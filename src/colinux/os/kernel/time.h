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

#endif

