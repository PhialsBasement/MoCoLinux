/*
 * This source code is a part of coLinux source package.
 *
 * The code is licensed under the GPL. See the COPYING file at
 * the root directory.
 */

#ifndef __COLINUX_KERNEL_VGPU_H__
#define __COLINUX_KERNEL_VGPU_H__

#include <colinux/common/common.h>
#include <colinux/kernel/manager.h>

/*
 * The host's half of the virtio-gpu transport ABI.
 *
 * The guest's copy is in patch/7.1.5/vgpu-src/virtio_colinux.c and there is
 * deliberately no shared header: the two sides are built by different
 * toolchains against different kernels, and a header that spanned them would
 * have to be reachable from both trees. What keeps them honest instead is that
 * both files carry compile-time assertions over every offset, so a change on
 * one side that is not mirrored on the other fails to build.
 *
 * Read the long comment at the top of virtio_colinux.c for why the transport
 * has no registers: nothing traps in this design, so a "kick" is an increment
 * of a shared counter that the host daemon spin-polls from another core, and
 * VIRTIO_F_ACCESS_PLATFORM is refused so that every vring address stays a
 * guest physical address the host can dereference in place.
 */

struct co_vgpu_vq {
	unsigned long long desc_gpa;	/* guest physical == host physical */
	unsigned long long avail_gpa;
	unsigned long long used_gpa;
	unsigned int	   num;
	unsigned int	   kick;	/* guest increments; daemon polls */
};

struct co_vgpu_io {
	unsigned int	   magic;	/* CO_VGPU_MAGIC, written by the host */
	unsigned int	   abi_version;
	unsigned long long host_features;
	unsigned long long guest_features;
	unsigned int	   status;	/* virtio status byte, guest-owned */
	unsigned int	   enabled;	/* daemon presence */
	unsigned int	   num_capsets;
	unsigned int	   num_scanouts;	/* zero: render-only */
	struct co_vgpu_vq  vq[2];		/* controlq, cursorq */
	unsigned int	   used_pending;	/* host bumps on publish */
	unsigned int	   reaped;		/* guest bumps on consume */
	unsigned int	   guest_heartbeat;
	unsigned int	   host_heartbeat;
};

#define CO_VGPU_MAGIC		0x55504756	/* 'VGPU' little-endian */
#define CO_VGPU_ABI_VERSION	1
#define CO_VGPU_NUM_VQS		2

/*
 * The features the host offers. VERSION_1 because nothing here speaks legacy
 * virtio, and VIRTIO_GPU_F_VIRGL because 3D is the entire point.
 *
 * ACCESS_PLATFORM is absent and must stay absent. Offering it would put the
 * guest's vring behind the DMA API, and every descriptor would then carry a
 * DMA address rather than a guest physical one -- which would cost a
 * translation the host has no way to perform, and would throw away the
 * zero-copy property that makes this design worth building.
 */
#define CO_VGPU_F_VERSION_1	(1ULL << 32)	/* VIRTIO_F_VERSION_1 */
#define CO_VGPU_F_VIRGL		(1ULL << 0)	/* VIRTIO_GPU_F_VIRGL */
#define CO_VGPU_F_CONTEXT_INIT	(1ULL << 4)	/* VIRTIO_GPU_F_CONTEXT_INIT */
#define CO_VGPU_F_RESOURCE_UUID	(1ULL << 2)
#define CO_VGPU_F_RESOURCE_BLOB	(1ULL << 3)
#define CO_VGPU_HOST_FEATURES	(CO_VGPU_F_VERSION_1 | CO_VGPU_F_VIRGL | \
				 CO_VGPU_F_RESOURCE_UUID | CO_VGPU_F_RESOURCE_BLOB | \
				 CO_VGPU_F_CONTEXT_INIT)

/*
 * Where the guest's structure lives, published by the loader once it has
 * resolved co_colinux_vgpu_io, and retired at teardown alongside the console
 * and network rings. Zero means "no GPU this run", which is a normal state.
 */
extern void		  co_vgpu_set_address(co_manager_t* manager,
					     unsigned long long va);
extern unsigned long long co_vgpu_address(void);

/*
 * Whether the guest has queued anything the daemon has not yet seen. Read by
 * the monitor loop's idle gate with no lock, beside co_net_rx_pending -- see
 * the comment on the implementation for why ordering rather than locking is
 * what makes that safe.
 */
extern bool_t		  co_vgpu_kick_pending(co_manager_t* manager);
extern co_rc_t		  co_vgpu_init(void);
extern void		  co_vgpu_free(void);

/*
 * Compile-time proof that this declaration matches the guest's.
 *
 * A negative array size is the C89 way of failing a build on a false
 * constant; the guest side uses BUILD_BUG_ON for the same offsets. Any change
 * to the layout has to be made in both places or one of the two stops
 * compiling, which is the point.
 */
typedef char co_vgpu_abi_check[
	(sizeof(struct co_vgpu_vq) == 32 &&
	 sizeof(struct co_vgpu_io) == 120 &&
	 __builtin_offsetof(struct co_vgpu_vq, desc_gpa)	== 0 &&
	 __builtin_offsetof(struct co_vgpu_vq, avail_gpa)	== 8 &&
	 __builtin_offsetof(struct co_vgpu_vq, used_gpa)	== 16 &&
	 __builtin_offsetof(struct co_vgpu_vq, num)		== 24 &&
	 __builtin_offsetof(struct co_vgpu_vq, kick)		== 28 &&
	 __builtin_offsetof(struct co_vgpu_io, magic)		== 0 &&
	 __builtin_offsetof(struct co_vgpu_io, abi_version)	== 4 &&
	 __builtin_offsetof(struct co_vgpu_io, host_features)	== 8 &&
	 __builtin_offsetof(struct co_vgpu_io, guest_features)	== 16 &&
	 __builtin_offsetof(struct co_vgpu_io, status)		== 24 &&
	 __builtin_offsetof(struct co_vgpu_io, enabled)		== 28 &&
	 __builtin_offsetof(struct co_vgpu_io, num_capsets)	== 32 &&
	 __builtin_offsetof(struct co_vgpu_io, num_scanouts)	== 36 &&
	 __builtin_offsetof(struct co_vgpu_io, vq)		== 40 &&
	 __builtin_offsetof(struct co_vgpu_io, used_pending)	== 104 &&
	 __builtin_offsetof(struct co_vgpu_io, reaped)		== 108)
	? 1 : -1];

#endif
