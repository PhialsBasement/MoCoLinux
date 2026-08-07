/*
 * This source code is a part of coLinux source package.
 *
 * Dan Aloni <da-x@colinux.org>, 2003 (c)
 *
 * The code is licensed under the GPL. See the COPYING file at
 * the root directory.
 *
 */

#ifndef __CO_OS_KERNEL_TIMER_H__
#define __CO_OS_KERNEL_TIMER_H__

#include <colinux/common/common.h>

typedef struct co_os_timer *co_os_timer_t;
typedef void (*co_os_func_t)(void *);

extern co_rc_t co_os_timer_create(co_os_func_t func, void *data,
				  long msec, co_os_timer_t *timer_out);
extern co_rc_t co_os_timer_activate(co_os_timer_t timer);
extern void co_os_timer_deactivate(co_os_timer_t timer);
extern void co_os_timer_destroy(co_os_timer_t timer);
extern void co_os_msleep(unsigned int msecs);

/*
 * The monitor loop's idle sleep, as something another process can cut short.
 *
 * co_os_msleep is a blind timeout: whatever happens during it waits for it.
 * That made the backoff tick the floor under every GPU fence -- the daemon
 * completes work with plain stores the monitor cannot see, so the guest slept
 * a full tick per completion. co_os_idle_wait sleeps the same bounded time but
 * wakes early when co_os_idle_wake is rung (from the daemon, through an
 * ioctl). Returns PTRUE if woken, PFALSE on timeout. The event is auto-reset:
 * one wake releases one wait, a wake with no waiter arms the next wait, and
 * nothing accumulates.
 *
 * One event per vCPU, because the event is auto-reset: a single event shared
 * by two sleeping vCPUs would release exactly one of them per ring, and which
 * one is the scheduler's business. That is not a fairness wrinkle, it is a
 * lost wake -- the vCPU with the work to do goes on sleeping. So a waiter
 * names itself, and a doorbell that does not know which vCPU wants the news
 * rings co_os_idle_wake_all().
 */
extern void   co_os_idle_wake_init(void);
extern void   co_os_idle_wake(unsigned long vcpu);
extern void   co_os_idle_wake_all(void);
extern bool_t co_os_idle_wait(unsigned long vcpu, unsigned int msecs);

typedef struct {
	union {
		struct {
			unsigned long low;
			unsigned long high;
		};
		unsigned long long quad;
	};
} co_timestamp_t;

extern void co_os_get_timestamp(co_timestamp_t *dts);
extern void co_os_get_timestamp_freq(co_timestamp_t *dts, co_timestamp_t *freq);
extern unsigned long co_os_get_cpu_khz(void);

#endif

