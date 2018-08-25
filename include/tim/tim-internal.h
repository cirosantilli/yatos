/* -*- mode: C; coding:utf-8 -*- */
/**********************************************************************/
/*  Yet Another Teachable Operating System                            */
/*  Copyright 2016 Takeharu KATO                                      */
/*                                                                    */
/*  Timer module internal definitions                                */
/*  Note: Following definitions must be used in a logical module only */
/*                                                                    */
/*                                                                    */
/**********************************************************************/
#if !defined(__TIMER_INTERNAL_H)
#define  __TIMER_INTERNAL_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include <kern/config.h>
#include <kern/kernel.h>
#include <kern/param.h>
#include <kern/assert.h>
#include <kern/kern_types.h>
#include <kern/timer.h>

struct _sync_obj;
struct _sync_block;
struct _timer_callout;
enum _sync_reason;

ticks _tim_refer_uptime_lockfree(void);
void _tim_invoke_callout(ticks _cur_tick);
void _tim_setup_uptime_clock(void);
#endif  /*  __TIMER_INTERNAL_H   */
