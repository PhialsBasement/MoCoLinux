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
 * msecs == 0 means NO timeout: wait until rung. NOTHING USES IT TODAY, and the
 * reason is worth stating so nobody reaches for it: the monitor loop's timeout
 * is not only a doorbell backstop, it is the guest's clock. Ticks are
 * synthesised from elapsed host time by co_colinux_take_ticks(), which runs
 * only when a vCPU's thread wakes and re-enters, so a vCPU that waits
 * indefinitely receives no ticks and every timer in that processor stops
 * expiring. It measured as cobd0 mounting at 13.173s instead of 0.348s.
 *
 * It stays because it becomes correct the moment the guest publishes its next
 * expiry instead of asking for a periodic tick -- a oneshot/NO_HZ clockevent,
 * where the host sleeps until exactly the next event and a doorbell is the only
 * other thing that can wake it.
 *
 * One event per vCPU, because the event is auto-reset: a single event shared
 * by two sleeping vCPUs would release exactly one of them per ring, and which
 * one is the scheduler's business. That is not a fairness wrinkle, it is a
 * lost wake -- the vCPU with the work to do goes on sleeping. So a waiter
 * names itself, and a doorbell that does not know which vCPU wants the news
 * rings co_os_idle_wake_all().
 */
extern void   co_os_idle_wake_init(void);
extern void   co_os_idle_wake_shutdown(void);
extern void   co_os_idle_wake(unsigned long vcpu);
extern void   co_os_idle_wake_all(void);
/*
 * Bind a vCPU's posted-interrupt DPC once, before the vCPU is published as
 * active.  Kicks then only queue that already-targeted object; teardown
 * unbinds and drains it before the slot can be reused on another processor.
 */
extern bool_t co_os_vcpu_kick_bind(unsigned long vcpu,
				   unsigned long host_cpu);
extern void   co_os_vcpu_kick_unbind(unsigned long vcpu);
extern void   co_os_vcpu_kick(unsigned long vcpu);
extern void   co_os_vcpu_preempt_start(unsigned long vcpu,
				       unsigned long host_cpu,
				       unsigned int period_msec);
extern void   co_os_vcpu_preempt_stop(unsigned long vcpu);
extern bool_t co_os_idle_wait(unsigned long vcpu, unsigned int msecs);
/*
 * Wait until an absolute host-monotonic deadline (100 ns units), waking
 * within ~50 us when the OS has high-resolution timers, one clock interrupt
 * late when it does not (XP). Doorbells cut it short like the ms wait.
 */
extern bool_t co_os_idle_wait_until(unsigned long vcpu,
				    unsigned long long abs_100ns);
extern int co_os_idle_hires_available(void);
/* Init-time self-measurement of a 100 us hires arm, in 100 ns units. */
extern unsigned long long co_os_idle_hires_selftest(void);

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
