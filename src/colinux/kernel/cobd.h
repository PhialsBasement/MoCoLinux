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
 * guest_pa is pseudo-physical. The driver resolves each page through the
 * loader's block index, so there is still no bounce buffer; the lookup also
 * confines the transfer to memory belonging to this guest.
 */
/*
 * The most descriptors one request may carry, matching BLK_MAX_SEGMENTS in the
 * guest's block layer. Bounded here because the count arrives from the guest
 * and is used to walk its memory.
 */
#define CO_COBD_MAX_SG	128

/*
 * The largest a single scatter-gather segment may claim to be.
 *
 * Matches max_segment_size in the guest's queue limits, so no honest request
 * ever approaches it. It exists because the length is read out of guest memory
 * and then used as a loop bound: a wrong one writes file contents across the
 * guest until it runs out of RAM, which is silent corruption when it lands on
 * data and a triple fault when it lands on kernel text.
 */
#define CO_COBD_MAX_SEGMENT	(1024 * 1024)

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
