/*
 * This source code is a part of coLinux source package.
 *
 * Dan Aloni <da-x@colinux.org>, 2003 (c)
 *
 * The code is licensed under the GPL. See the COPYING file at
 * the root directory.
 *
 */

#ifndef __CO_OS_KERNEL_MISC_H__
#define __CO_OS_KERNEL_MISC_H__

#include <colinux/common/common.h>

/*
 * A host physical address. Wider than a host virtual address can be: PAE gives
 * i386 36 bits, and on x86-64 a Win64 unsigned long is only 32 bits, so
 * returning unsigned long here silently truncated every physical address above
 * 4 GB -- including the passage page's own, which the world switch loads into
 * CR3.
 */
typedef unsigned long long co_pa_t;

extern co_pa_t co_os_virt_to_phys(void *addr);
extern co_rc_t co_os_physical_memory_pages(unsigned long *pages);
extern co_id_t co_os_current_id(void);

#endif
