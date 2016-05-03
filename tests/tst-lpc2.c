/* -*- mode: C; coding:utf-8 -*- */
/**********************************************************************/
/*  Yet Another Teachable Operating System                            */
/*  Copyright 2016 Takeharu KATO                                      */
/*                                                                    */
/*  LPC test routines                                                 */
/*                                                                    */
/**********************************************************************/

#include <stddef.h>
#include <stdint.h>

#include <kern/config.h>
#include <kern/assert.h>
#include <kern/string.h>
#include <kern/kprintf.h>
#include <kern/thread.h>
#include <kern/sched.h>
#include <kern/proc.h>
#include <kern/lpc.h>

#include <kern/tst-progs.h>

static thread *thrA, *thrB;
int
kthreadA(void __attribute__ ((unused)) *arg) {
	msg_body msg;
	int rc;

	msg.sys_pri_dbg_msg.msg="Hello World";
	msg.sys_pri_dbg_msg.len = strlen(msg.sys_pri_dbg_msg.msg);
	
	kprintf(KERN_INF, "ThreadA:tid=%d thread=%p\n", 
	    current->tid, current);
	kprintf(KERN_INF, "ThreadA send(%d,1000ms, msg)\n", thrB->tid);

	rc = lpc_send_and_reply(thrB->tid, &msg);
	kprintf(KERN_INF, "ExitThreadA: send-rc = %d reply-rc=%d\n", rc, 
	    msg.sys_pri_dbg_msg.rc);
	thr_exit(0);

	return 0;
}

int
kthreadB(void __attribute__ ((unused)) *arg) {
	endpoint src;
	msg_body msg;
	int rc;

	kprintf(KERN_INF, "ThreadB:tid=%d thread=%p\n", 
	    current->tid, current);
	rc = lpc_recv(LPC_RECV_ANY, LPC_INFINITE, &msg, &src);
	kprintf(KERN_INF, "ThreadB:recv rc=%d src=%u msg=%s len=%d\n",
	    rc, src, msg.sys_pri_dbg_msg.msg, msg.sys_pri_dbg_msg.len);

	msg.sys_pri_dbg_msg.rc = msg.sys_pri_dbg_msg.len;
	rc = lpc_send(src, LPC_INFINITE, &msg);
	kprintf(KERN_INF, "ExitThreadB:reply rc=%d \n", rc);

	return 0;
}


void
lpc2_test(void) {
	int              rc;

	rc = thr_new_thread(&thrA);
	kassert( rc == 0 );

	rc = thr_new_thread(&thrB);
	kassert( rc == 0 );

	rc = thr_create_kthread(thrA, 0, THR_FLAG_NONE, 
	    THR_INVALID_TID, kthreadA, (void *)"ThreadA");
	kassert( rc == 0 );

	rc = thr_create_kthread(thrB, 0, THR_FLAG_NONE, 
	    THR_INVALID_TID, kthreadB, (void *)"ThreadB");
	kassert( rc == 0 );

	rc = thr_start(thrA, current->tid);
	kassert( rc == 0 );

	rc = thr_start(thrB, current->tid);
	kassert( rc == 0 );
}

