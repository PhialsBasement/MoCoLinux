/*
 * This source code is a part of coLinux source package.
 *
 * The code is licensed under the GPL. See the COPYING file at
 * the root directory.
 */

#ifndef __COLINUX_KERNEL_COBD_H__
#define __COLINUX_KERNEL_COBD_H__

#include <colinux/common/common.h>
#include <colinux/common/ioctl.h>
#include <colinux/kernel/manager.h>

/*
 * The cooperative block device, host side.
 *
 * Four units is what the bring-up needs -- a root filesystem and somewhere to
 * put the results -- not a limit anything depends on. CO_COBD_MAX_UNITS is
 * defined in common/ioctl.h, because the daemon sizes its own --cobdN argument
 * list from it and the two must agree.
 */

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
/*
 * The most descriptors one request may carry, matching BLK_MAX_SEGMENTS in the
 * guest's block layer. Bounded here because the count arrives from the guest
 * and is used to walk its memory.
 */
#define CO_COBD_MAX_SG	128

/*
 * A whole request in one crossing: `count` descriptors at guest physical
 * address `sg_pa`, each a physical run and a length, written or read in order
 * from `offset`. See the comment in cobd.c.
 */
extern co_rc_t co_cobd_request_sg(co_manager_t* manager, int unit,
				  unsigned long long offset,
				  unsigned long long sg_pa,
				  unsigned int count, bool_t write);

extern co_rc_t co_cobd_request(co_manager_t* manager, int unit,
			       unsigned long long offset,
			       unsigned long long guest_pa,
			       unsigned long size, bool_t write);

#endif
