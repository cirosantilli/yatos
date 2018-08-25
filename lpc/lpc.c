/* -*- mode: C; coding:utf-8 -*- */
/**********************************************************************/
/*  Yet Another Teachable Operating System                            */
/*  Copyright 2016 Takeharu KATO                                      */
/*                                                                    */
/*  Local process communication routines                              */
/*                                                                    */
/**********************************************************************/

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include <kern/config.h>
#include <kern/kernel.h>
#include <kern/param.h>
#include <kern/kern_types.h>
#include <kern/assert.h>
#include <kern/kprintf.h>
#include <kern/string.h>
#include <kern/errno.h>
#include <kern/spinlock.h>
#include <kern/queue.h>
#include <kern/list.h>
#include <kern/proc.h>
#include <kern/vm.h>
#include <kern/thread.h>
#include <kern/sched.h>
#include <kern/timer.h>
#include <kern/page.h>
#include <kern/lpc.h>

#include <thr/thr-internal.h>
#include <tim/tim-internal.h>

/** メッセージキューが空であることを確認する(ロック獲得なし版)
    @param[in] que 調査対象のメッセージキュー
    @retval true メッセージキューが空
    @retval false メッセージキューが空でない
 */
bool
is_msg_queue_empty_nolock(msg_queue *que) {
	
	kassert( que != NULL );
	kassert( spinlock_locked_by_self( &que->lock ) );

	return queue_is_empty( &que->que );
}

/** メッセージをキューに追加する
    @param[in] que 追加対象のメッセージキュー
    @param[in] m   追加対象のメッセージ
    @retval  0      正常に追加した
    @retval -EINVAL メッセージの状態が不正
 */
static int
lpc_msg_add_nolock(msg_queue *que, msg *m){
	intrflags flags;

	kassert( que != NULL );
	kassert( m != NULL );
	kassert( spinlock_locked_by_self( &que->lock ) );

	hal_cpu_disable_interrupt( &flags );
	queue_add( &que->que, &m->link);
	m->qp = que;
	hal_cpu_restore_interrupt( &flags );
	
	return 0;
}

/** メッセージ用のメモリを獲得する
    @param[in] mp       メッセージ領域のアドレスを格納するポインタ変数のアドレス
    @param[in] pgflags  メモリ獲得方針
    @retval    0        正常に獲得できた
    @retval   -ENOMEM   メモリ獲得に失敗した
 */
static int
lpc_msg_alloc(msg **mp, pgalloc_flags pgflags) {
	msg   *m;

	kassert( mp != NULL );

	m = (msg *)kmalloc(sizeof(msg), pgflags);
	if ( m == NULL ) 
		return -ENOMEM;

	/*
	 * リンク, 状態, 送信完了同期オブジェクトを初期化
	 */
	list_init( &m->link );
	m->qp = NULL;
	sync_init_object( &m->completion, SYNC_WAKE_FLAG_ALL, THR_TSTATE_WAIT );
	spinlock_init( &m->cqlock );

	m->src = current->tid;  /*  送信元エンドポイントを自スレッドに設定  */
	*mp = m;

	return 0;
}

/** メッセージ領域を開放する
    @param[in] m 開放対象のメッセージ領域
 */
static void
lpc_msg_free(msg *m) {
	intrflags flags;
	
	if ( m == NULL )
		return;

	spinlock_lock_disable_intr( &m->cqlock, &flags);
	sync_wake( &m->completion, SYNC_OBJ_DESTROYED);
	spinlock_unlock_restore_intr( &m->cqlock, &flags);

	kfree( m );
}

/** メッセージを取り出す
    @param[in] src   送信元エンドポイント
    @param[in] mque   メッセージキュー
    @return 非NULL 受信可能な最初のメッセージ
    @return NULL   受信可能なメッセージがない
 */
static msg *
dequeue_message_nolock(endpoint src, msg_queue *mque) {
	list *li, *next;
	msg *m;

	kassert( mque != NULL );
	kassert( spinlock_locked_by_self( &mque->lock ) );	

	for ( li = queue_ref_top( &mque->que );
	      li != (list *)(&mque->que);
	      li = next) {

		next = li->next;
		m = CONTAINER_OF(li, struct _msg, link);
		if ( ( src == LPC_RECV_ANY ) || ( m->src == src ) ) {
			
			list_del(li);  /*  メッセージを取り出す  */
			return m;
		}
	}

	return NULL;
}

/** メッセージキューを初期化する
    @param[in] que 初期化対象のメッセージキュー
 */
void
lpc_msg_queue_init(msg_queue *que) {

	kassert( que != NULL );

	spinlock_init( &que->lock );
	queue_init( &que->que );
	sync_init_object( &que->wait_sender, SYNC_WAKE_FLAG_ALL, THR_TSTATE_WAIT);
	sync_init_object( &que->wait_reciever, SYNC_WAKE_FLAG_ALL, THR_TSTATE_WAIT);
}

/** メッセージキューを破棄する
    @param[in] que 操作対象のメッセージキュー
 */
void
lpc_destroy_msg_queue(msg_queue *que) {
	intrflags flags;
	list        *li;
	list      *next;
	msg          *m;

	kassert( que != NULL );
	kassert( !check_recursive_locked( &que->lock ) );

	spinlock_lock_disable_intr( &que->lock, &flags);
	for( li = queue_ref_top(&que->que);
	     li != (list *)&que->que;
	     li = next ) {

		next = li->next;

		list_del(li);  /*  メッセージを取り出す  */
		m = CONTAINER_OF(li, struct _msg, link);
		lpc_msg_free(m); /* メッセージを開放する  */
	}
	sync_wake( &que->wait_reciever, SYNC_OBJ_DESTROYED ); /* メッセージキュー消滅を通知 */
	sync_wake( &que->wait_sender, SYNC_OBJ_DESTROYED );  /* メッセージキュー消滅を通知 */
	spinlock_unlock_restore_intr( &que->lock, &flags);
}


/** メッセージを送信する
    @param[in] dest  送信先エンドポイント
    @param[in] tmout タイムアウト時間(単位:ms)
    @param[in] m     送信電文
    @retval    0     正常に送信した
    @retval   -EINTR イベント割込み
    @note 受信側が送信待ちキューで待機するまでメッセージの書き込みを待ち合わせ、
    電文を登録してから送信待ちキューを起床することで受信側が起床したときには
    メッセージが存在することを保証する
 */
int
lpc_send(endpoint dest, lpc_tmout tmout, void *m){
	int               rc;
	intrflags      flags;
	thread          *thr;
	msg_queue         *q;
	msg         *new_msg;
	sync_reason      res;

	kassert( m != NULL );

	acquire_all_thread_lock( &flags );
	thr = thr_find_thread_by_tid_nolock(dest);
	if ( thr == NULL ) {

		release_all_thread_lock(&flags);
		return -ENOENT;  /*  宛先不明  */
	}

	q = &thr->mque;

	rc = lpc_msg_alloc(&new_msg, KMALLOC_NORMAL );
	if ( rc != 0 )  { /*  メモリ獲得失敗  */

		release_all_thread_lock(&flags);
		return rc;
	}

	/*
	 * 送信待ちスレッドがいない場合は, 受信側スレッドを待ち合わせる
	 */
	spinlock_lock( &q->lock );
	release_all_thread_lock(&flags);

	while( queue_is_empty( &q->wait_sender.que ) ){
		
		if ( tmout == 0 ) {
			
			/* ノンブロッキング送信
			 * 受信者がいない場合は, ロック, メモリを解放して抜ける
			 */
			rc = -EAGAIN;
			goto msg_free_out;
		} else if ( tmout < 0 ) {

			/* ブロッキング送信
			 * 受信者待ち同期オブジェクトで休眠
			 */
			res = sync_wait(&q->wait_reciever, &q->lock);
			if ( res == SYNC_OBJ_DESTROYED ) {

                                /*  スレッド破棄に伴ってキューが消失
				 *  したためメモリだけ解放して抜ける
				 */
				rc = -ENOENT;
				goto msg_free_out;
			}

			if ( res == SYNC_WAI_DELIVEV ) {

                                /*  イベント受信時は, メモリを解放して抜ける
				 */
				rc = -EINTR;
				goto msg_free_out;
			}
		} else {
			
			/* タイムアウト付きのブロッキング送信
			 * 受信者待ちとタイムアウトの2つの同期オブジェクトに
			 * 対して休眠する
			 */
			res = tim_wait_obj(&q->wait_reciever, tmout, &q->lock );
			if ( res == SYNC_OBJ_DESTROYED ) {

                                /*  スレッド破棄に伴ってキューが消失
				 *  するためメモリだけ解放して抜ける
				 */
				rc = -ENOENT;
				goto msg_free_out;
			}

			if ( res == SYNC_WAI_TIMEOUT ) {

                                /*  タイムアウト時は, メモリを解放して抜ける
				 */
				rc = -EAGAIN;
				goto msg_free_out;
			}

			if ( res == SYNC_WAI_DELIVEV ) {

                                /*  イベント受信時は, メモリを解放して抜ける
				 */
				rc = -EINTR;
				goto msg_free_out;
			}

			/* 待ち中は, 全スレッドロックを取っていないので, 
			 * 送信先スレッドが消失する可能性がある。
			 * 送信先スレッド消失は, 上記のオブジェクト破棄
			 * で通知されるはずなのでアサーションとして扱う。
			 */
			acquire_all_thread_lock( &flags );
			thr = thr_find_thread_by_tid_nolock(dest);
			release_all_thread_lock(&flags);
			kassert ( thr != NULL );
		}
	}

	kassert( spinlock_locked_by_self( &q->lock ) );
	kassert( !all_thread_locked_by_self() );

	/*  メッセージをユーザ空間からコピーする  */
	rc = vm_copy_in(&current->p->vm, &new_msg->body, m, sizeof(msg_body));
	if ( rc == -EFAULT )
		goto unlock_out;

	lpc_msg_add_nolock(q, new_msg);  /*  メッセージキューに追加  */

	spinlock_unlock(&q->lock);        /* キューロックを開放  */

        /*
	 *  送信完了待ち
	 */
	spinlock_lock(&new_msg->cqlock);  /* 受信者が起床処理を待ち合わせるように
					   *  送信完了待ちキューをロック
					   */
	sync_wake( &q->wait_sender, SYNC_WAI_RELEASED);  /*  送信待ち受信者を起床  */
	/*  送信完了待ち同期オブジェクトでの休眠  */
	res = sync_wait(&new_msg->completion, &new_msg->cqlock);

	spinlock_unlock(&new_msg->cqlock);  /* 送信完了待ちキューをアンロック */

	if ( res == SYNC_OBJ_DESTROYED )   /* メッセージ破棄による起床  */
		return  -ENOENT;

	if ( res == SYNC_WAI_DELIVEV )
		return -EINTR;  /*  イベント受信による起床  */

	return 0;

unlock_out:
	kassert( spinlock_locked_by_self( &q->lock ) );
	kassert( !all_thread_locked_by_self() );

	spinlock_unlock( &q->lock );

	return rc;

msg_free_out:
	kassert( spinlock_locked_by_self( &q->lock ) );
	kassert( !all_thread_locked_by_self() );

	lpc_msg_free(new_msg);
	spinlock_unlock( &q->lock );

	return rc;
}

/** メッセージを受信する
    @param[in]     src     送信元エンドポイント
    @param[in]     tmout   タイムアウト時間(単位:ms)
    @param[in]     m       受信電文格納先
    @param[in,out] msg_src 受信したメッセージの送信元エンドポイント格納先
    @retval    0       正常に受信した
    @retval   -EAGAIN  電文がなかった
    @note 
 */
int
lpc_recv(endpoint src, lpc_tmout tmout, void *m, endpoint *msg_src){
	int               rc;
	intrflags      flags;
	msg            *rmsg;
	sync_reason      res;

	spinlock_lock_disable_intr( &current->mque.lock, &flags);

	while(1) {

		/*  送信者を起床  */
		sync_wake( &current->mque.wait_reciever, SYNC_WAI_RELEASED ); 

		/*  キューからメッセージを取り出す  */		
		rmsg = dequeue_message_nolock(src, &current->mque);
		if ( rmsg != NULL )
			break;

		if ( tmout == 0 ) {
			
			/* ノンブロッキング受信
			 * 受信可能なメッセージがない場合は, エラー復帰
			 */
			rc = -EAGAIN;
			goto unlock_out;
		} else if ( tmout < 0 ) {
			
			/* ブロッキング受信
			 * 送信者待ち同期オブジェクトで休眠
			 */
			res = sync_wait( &current->mque.wait_sender, 
			    &current->mque.lock );
			/*  自スレッドのキューであるためオブジェクト破壊
			 *  で返ることはないためアサーションを先に判定
			 */
			kassert( res != SYNC_OBJ_DESTROYED );
                         /* メッセージキュー破棄による復帰  */
			if ( res == SYNC_OBJ_DESTROYED ) {

				rc =  -ENOENT;
				goto unlock_out;
			}
			/*  イベント受信による復帰  */
			if ( res == SYNC_WAI_DELIVEV ) {

				rc = -EINTR;
				goto unlock_out;
			}
		} else {

			/* タイムアウト付きのブロッキング受信
			 * 送信者待ちとタイムアウトの2つの同期オブジェクトに
			 * 対して休眠する
			 */
			res = tim_wait_obj(&current->mque.wait_sender, tmout, 
			    &current->mque.lock );
			/*  自スレッドのキューであるためオブジェクト破壊
			 *  で返ることはない
			 */
			kassert( res != SYNC_OBJ_DESTROYED );

			if ( res == SYNC_WAI_TIMEOUT ) {

				rc = -EAGAIN;  /*  タイムアウトによる復帰  */
				goto unlock_out;
			}
			if ( res == SYNC_WAI_DELIVEV ) {

				rc = -EINTR;  /*  イベント受信による復帰  */
				goto unlock_out;
			}
		}
		
	}

	/*  メッセージをユーザ空間にコピーする  */	
	rc = vm_copy_out(&current->p->vm, m, &rmsg->body, sizeof(msg_body));
	if ( rc == -EFAULT )
		goto wakeup_out;

	if ( msg_src != NULL ) {
		
		/*  送信者のスレッドIDを返却する  */	
		rc = vm_copy_out(&current->p->vm, msg_src, &rmsg->src, sizeof(endpoint) );
		if ( rc == -EFAULT )
			goto wakeup_out;
	}

	rc = 0;

wakeup_out:
	/*
	 *  送信完了待ち合わせ処理
	 */
	spinlock_lock( &rmsg->cqlock );
	sync_wake( &rmsg->completion, SYNC_WAI_RELEASED );  /*  送信者を起床  */
	spinlock_unlock( &rmsg->cqlock );
	
unlock_out:
	spinlock_unlock_restore_intr( &current->mque.lock, &flags);
	
	return rc;
}

/** メッセージを送信し返信を待ち受ける
    @param[in] dest   送信先エンドポイント
    @param[in] m      受信電文格納先
    @retval    0      正常に受信した
    @note 送信後ユーザ空間に戻らず, リプライを受け付けるシステムコール
    典型的な送受信手順になるので, 複合システムコール(composite system call)
    として用意している。
 */
int
lpc_send_and_reply(endpoint dest, void *m) {
	int rc;

	rc = lpc_send(dest, LPC_INFINITE, m);
	if ( rc != 0 )
		return rc;
	
	rc = lpc_recv(dest, LPC_INFINITE, m, NULL);
	if (rc != 0 )
		return rc;

	return 0;
}
