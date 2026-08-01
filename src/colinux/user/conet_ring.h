/*
 * This source code is a part of coLinux source package.
 *
 * The code is licensed under the GPL. See the COPYING file at
 * the root directory.
 */

#ifndef __COLINUX_USER_CONET_RING_H__
#define __COLINUX_USER_CONET_RING_H__

#include <colinux/common/common.h>
#include <colinux/os/user/manager.h>

/*
 * The guest's network rings, from the host side.
 *
 * The layout is ABI, declared in the guest driver (drivers/net/conet_colinux.c)
 * and mirrored by the driver's kernel/net.c. Records are a 32-bit length, the
 * frame, and padding to a four-byte boundary: every record starts aligned, so
 * the length word never straddles the wrap and both sides read it in one piece.
 *
 * These live here rather than inside one tool because two programs need them --
 * the boot daemon's post-run dump and the slirp bridge -- and a decoder that
 * exists twice is one that disagrees with itself the first time only one copy
 * gets fixed.
 */
#define CO_NETIO_TX_HEAD	0x00
#define CO_NETIO_TX_TAIL	0x04
#define CO_NETIO_RX_HEAD	0x08
#define CO_NETIO_RX_TAIL	0x0c
#define CO_NETIO_TX		0x10
#define CO_NETIO_TX_SIZE	(128 * 1024)
#define CO_NETIO_RX_SIZE	(128 * 1024)
#define CO_NETIO_MAX_FRAME	1514

extern unsigned int co_net_le32(const unsigned char* p);

/*
 * A snapshot of the four indices plus the TX bytes the guest has published.
 *
 * Only tail..head is fetched, clamped so no request straddles the wrap: each
 * ioctl call is a METHOD_BUFFERED round trip and therefore a non-paged pool
 * allocation in the driver, so reading all 128 KB to look at a few hundred
 * bytes is pool churn for nothing. The buffer must be CO_NETIO_TX_SIZE bytes;
 * frames are written at their own masked offsets, exactly as the guest wrote
 * them, so the caller indexes it the way the guest does.
 *
 * The snapshot is consistent without a lock held across the calls: the guest
 * only appends, the host is the only writer of tx_tail, and a full ring drops
 * rather than overwriting -- so every byte in tail..head is immutable once
 * published.
 */
extern co_rc_t co_net_fetch(co_manager_handle_t handle,
			    unsigned int* tx_head, unsigned int* tx_tail,
			    unsigned int* rx_head, unsigned int* rx_tail,
			    unsigned char* ring);

/*
 * Walk the records in a fetched snapshot, calling back with each frame.
 *
 * Returns the number of frames visited, or -1 if a record could not be a frame
 * -- a length of zero, one longer than an ethernet frame, or one reaching past
 * what the guest published. A desynchronised ring is reported rather than
 * resynchronised, because there is no way to find the next record boundary
 * from inside the corruption and a decoder that guesses produces convincing
 * nonsense.
 *
 * `consumed_out` receives the absolute position after the last frame the
 * callback accepted, which is what the caller passes to CONET_TAKE. A callback
 * returning false stops the walk with that frame left unconsumed, so it is
 * offered again next time -- which is how back-pressure from a full RX ring is
 * expressed.
 */
typedef bool_t (*co_net_frame_func_t)(void* data, const unsigned char* frame,
				     unsigned int len);

extern int co_net_walk(const unsigned char* ring,
		       unsigned int tx_head, unsigned int tx_tail,
		       co_net_frame_func_t func, void* data,
		       unsigned int* consumed_out);

/* One line naming a frame's addresses and protocol, for the dumps. */
extern void co_net_describe_frame(const unsigned char* f, unsigned int len);

#endif
