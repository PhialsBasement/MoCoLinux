/*
 * This source code is a part of coLinux source package.
 *
 * Dan Aloni <da-x@colinux.org>, 2003 (c)
 *
 * The code is licensed under the GPL. See the COPYING file at
 * the root directory.
 *
 */

#ifndef __CO_OS_KERNEL_USER_H__
#define __CO_OS_KERNEL_USER_H__

#include <colinux/common/common.h>
#include <colinux/arch/mmu.h>	/* co_pfn_t, for the locking helper below */

/*
 * Userspace access routines.
 */

co_rc_t co_copy_to_user(char *user_address, char *kernel_address, unsigned long size);
co_rc_t co_copy_from_user(char *user_address, char *kernel_address, unsigned long size);

/*
 * Lock a caller's pages and report the machine frames behind them, keeping
 * them locked until the handle is released.
 *
 * For memory that is about to be published in the guest's p2m: the guest ends
 * up holding page-table entries naming these frames, so they must not move.
 * The frame array is owned by the call and freed with the handle.
 */
co_rc_t co_os_user_lock_pages(void *user_address, unsigned long size,
			      void **handle_out, co_pfn_t **pfns_out,
			      unsigned long *count_out);
void	co_os_user_unlock_pages(void *handle);

/*
 * Lock a block of a caller's section view AND map it at kernel mode, so the
 * driver can reach it from any process context. The kernel mapping is what
 * makes section-backed guest RAM equivalent to the old pool blocks for every
 * in-driver reader; the KernelMode map returns NULL on failure rather than
 * raising, which is the property the section design rests on. Size is capped
 * by what one MDL can describe (just under 16 MB).
 */
co_rc_t co_os_user_block_map(void *user_address, unsigned long size,
			     void **kernel_va_out, void **handle_out);
void	co_os_user_block_unmap(void *kernel_va, void *handle);

#endif
