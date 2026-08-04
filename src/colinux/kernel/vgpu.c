/*
 * The host half of the guest's virtio-gpu transport.
 *
 * Small on purpose, and it will stay small. Almost everything about this
 * device happens in userspace -- the daemon that embeds virglrenderer polls
 * the kick counter from another core, walks the vrings through the persistent
 * mappings R3 established, and writes completions back. None of that needs the
 * driver, because guest physical memory is host physical memory and a mapped
 * page is just a page.
 *
 * What the driver owns is exactly two things the daemon cannot do for itself:
 *
 *   1. Publishing the address of the guest's co_colinux_vgpu_io, which only
 *      the loader knows, and retiring it at teardown. That retirement is the
 *      same discipline the console and network rings needed, and it was bought
 *      the same way -- a cross-process reader walking page tables that another
 *      thread had already freed is a bugcheck, not a wrong answer.
 *
 *   2. Answering "has the guest queued anything?" for the monitor loop's idle
 *      gate, so a guest that submits work and then goes idle does not sit
 *      there while the daemon waits for a kick it cannot see.
 *
 * The rings themselves are never touched here.
 */

#include <colinux/common/common.h>
#include <colinux/common/libc.h>
#include <colinux/kernel/manager.h>
#include <colinux/kernel/kload.h>
#include <colinux/os/kernel/mutex.h>

#include "vgpu.h"

static unsigned long long vgpu_io_va;
static co_os_mutex_t	  vgpu_lock;

/*
 * A cached pointer straight at the guest's kick counters.
 *
 * The idle gate in the monitor loop runs on every yield and cannot afford a
 * page-table walk, let alone a mutex -- the network path's mutex-per-yield is
 * explicitly not copied here. So the frame is resolved once, when the address
 * is published, and read with no lock afterwards.
 *
 * What makes that safe is ordering rather than locking, and it is worth being
 * precise about because getting it wrong reopens the 0xD5 class. The pointer
 * is only ever cleared under the lock, in the same retirement that runs before
 * co_kload_free releases anything; R3's deferred release then keeps the
 * underlying pages alive past any reader that was already inside the gate. If
 * that ordering is ever changed, this pointer becomes a dangling read.
 */
static volatile unsigned int* vgpu_kick_frame[CO_VGPU_NUM_VQS];

co_rc_t co_vgpu_init(void)
{
	return co_os_mutex_create(&vgpu_lock);
}

void co_vgpu_free(void)
{
	if (vgpu_lock) {
		co_os_mutex_destroy(vgpu_lock);
		vgpu_lock = NULL;
	}
}

/*
 * Publish, or retire with zero.
 *
 * Called by the loader once co_colinux_vgpu_io has been resolved, and by
 * co_kload_free -- beside co_console_set_address and co_net_set_address --
 * before anything is torn down.
 */
void co_vgpu_set_address(co_manager_t* manager, unsigned long long va)
{
	int i;

	co_os_mutex_acquire(vgpu_lock);

	vgpu_io_va = va;

	for (i = 0; i < CO_VGPU_NUM_VQS; i++)
		vgpu_kick_frame[i] = NULL;

	if (va != 0) {
		/*
		 * Resolve each kick counter to a host virtual address now,
		 * while the guest's tables are certainly alive. co_kload_read
		 * walks them; doing that once here is what buys the lock-free
		 * read in the idle gate below.
		 */
		for (i = 0; i < CO_VGPU_NUM_VQS; i++) {
			unsigned long long kick_va =
				va + __builtin_offsetof(struct co_vgpu_io, vq) +
				(unsigned long long)i * sizeof(struct co_vgpu_vq) +
				__builtin_offsetof(struct co_vgpu_vq, kick);

			vgpu_kick_frame[i] =
				(volatile unsigned int*)co_kload_host_ptr(manager, kick_va);
		}
	}

	co_os_mutex_release(vgpu_lock);
}

unsigned long long co_vgpu_address(void)
{
	unsigned long long va;

	co_os_mutex_acquire(vgpu_lock);
	va = vgpu_io_va;
	co_os_mutex_release(vgpu_lock);

	return va;
}

/*
 * Has the guest kicked since the daemon last looked?
 *
 * Deliberately approximate. This does not compare against what the daemon has
 * consumed -- it cannot, that lives in the daemon -- it reports whether the
 * counter has moved since the last time this function saw it. A false positive
 * costs one extra pass through the monitor loop; a false negative would cost a
 * stalled submission, so the bias is towards saying yes.
 */
bool_t co_vgpu_kick_pending(co_manager_t* manager)
{
	static unsigned int last_seen[CO_VGPU_NUM_VQS];
	bool_t		    moved = PFALSE;
	int		    i;

	for (i = 0; i < CO_VGPU_NUM_VQS; i++) {
		volatile unsigned int* p = vgpu_kick_frame[i];
		unsigned int	       now;

		if (p == NULL)
			continue;

		now = *p;
		if (now != last_seen[i]) {
			last_seen[i] = now;
			moved	     = PTRUE;
		}
	}

	return moved;
}
