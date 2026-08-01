/*
 * This source code is a part of coLinux source package.
 *
 * The code is licensed under the GPL. See the COPYING file at
 * the root directory.
 */

#ifndef __COLINUX_KERNEL_COBD_H__
#define __COLINUX_KERNEL_COBD_H__

#include <colinux/common/common.h>
#include <colinux/kernel/manager.h>

/*
 * The cooperative block device, host side.
 *
 * Four units is what the bring-up needs -- a root filesystem and somewhere to
 * put the results -- not a limit anything depends on.
 */
#define CO_COBD_MAX_UNITS	4

extern co_rc_t co_cobd_attach(int unit, const char* path,
			      unsigned long long* size_out);
extern void    co_cobd_detach_all(void);

extern bool_t		 co_cobd_present(int unit);
extern unsigned long long co_cobd_size(int unit);

/*
 * One transfer, from the guest's point of view: read or write `size` bytes at
 * `offset` against the unit's backing store, into or out of guest physical
 * memory at `guest_pa`.
 *
 * Guest physical is host physical, so the buffer is memory this driver
 * allocated and can address directly -- there is no bounce buffer and no
 * mapping step. The address is resolved through the same frame lookup the
 * page-table builder uses, which is also what confines the transfer to memory
 * belonging to the guest.
 */
extern co_rc_t co_cobd_request(co_manager_t* manager, int unit,
			       unsigned long long offset,
			       unsigned long long guest_pa,
			       unsigned long size, bool_t write);

#endif
