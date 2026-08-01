/*
 * This source code is a part of coLinux source package.
 *
 * The code is licensed under the GPL. See the COPYING file at
 * the root directory.
 */

#ifndef __COLINUX_KERNEL_NET_H__
#define __COLINUX_KERNEL_NET_H__

#include <colinux/common/common.h>
#include <colinux/kernel/manager.h>

extern co_rc_t co_net_init(void);
extern void    co_net_free(void);

/* Enable the rings for a guest, or -- with zero -- retire them. */
extern void co_net_set_address(unsigned long long va);

/*
 * Read-only view of the TX ring for a live guest: the four indices and a
 * window of ring bytes starting at absolute position `start`. Writes nothing
 * into the guest, not even tx_tail.
 */
/*
 * Advance tx_tail to new_tail -- the host's consume. Validated under the
 * lock: only forward, at most to the current head. INVALID_PARAMETER names
 * a refused tail; nothing is written on refusal.
 */
extern co_rc_t co_net_take(co_manager_t* manager, unsigned int new_tail,
			   unsigned int* tx_head, unsigned int* tx_tail,
			   unsigned int* rx_head, unsigned int* rx_tail);

extern co_rc_t co_net_dump(co_manager_t* manager,
			   unsigned int* tx_head, unsigned int* tx_tail,
			   unsigned int* rx_head, unsigned int* rx_tail,
			   unsigned int start, unsigned char* data,
			   unsigned int* size);

#endif
