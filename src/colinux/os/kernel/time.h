/*
 * This source code is a part of coLinux source package.
 *
 * Dan Aloni <da-x@colinux.org>, 2003 (c)
 *
 * The code is licensed under the GPL. See the COPYING file at
 * the root directory.
 *
 */

#ifndef __CO_OS_KERNEL_TIME_H__
#define __CO_OS_KERNEL_TIME_H__

/*
 * Obtains the GMT time_t from the host.
 */

extern unsigned long co_os_get_time(void);

/*
 * The host's monotonic clock in 100 ns units, for feeding the guest virtual
 * time. Monotonic rather than wall time: it never steps, so tick arithmetic
 * against a remembered value cannot go backwards or explode across a host
 * clock adjustment.
 */
extern unsigned long long co_os_monotonic_100ns(void);

/*
 * Ask the host for a finer clock for the duration of a run, and give it back.
 *
 * Sleeps are rounded up to the host's clock tick, so that tick is the floor on
 * every round trip a guest makes: the monitor loop sleeps when the guest is
 * idle, and a guest waiting for a reply is idle. On XP the default tick is
 * 15.6 ms, which is why a ping to slirp's own gateway -- answered on this same
 * machine, no network involved -- measures 10 ms, and why a download stalls at
 * the same rate whether the mirror is 5 ms away or on another continent.
 *
 * acquire() returns the resolution actually granted in 100 ns units, which is
 * not necessarily the one asked for; the caller reports what it got. Release
 * is reference counted by the host and must be paired, because a faster clock
 * costs the whole machine power and interrupts.
 */
extern unsigned long co_os_timer_resolution_acquire(void);
extern void	     co_os_timer_resolution_release(void);

#endif

