/*
 * This source code is a part of coLinux source package.
 *
 * The code is licensed under the GPL. See the COPYING file at
 * the root directory.
 */

/*
 * Asynchronous cooperative block I/O, host side. See doc/async-cobd-design and
 * cobd_async.h. This is the fix for defect 3: the transfer moves off the
 * CPU-pinned monitor thread onto kernel worker threads, so the single-CPU guest
 * keeps running while its disk I/O is in flight.
 *
 * The transfer itself is unchanged -- co_cobd_request_sg (cobd.c) walks the
 * guest's scatter list a page at a time through co_kload_frame_va and validates
 * every bound. It is a pure function of (manager, request) with no writable
 * global state, so running it on a worker instead of the monitor thread is
 * safe: distinct requests touch distinct guest pages and distinct file offsets,
 * and the backing handle serializes itself.
 *
 * The completion ring (co_colinux_cobd_io in guest .bss) is written the way the
 * net RX ring is (net.c): the record is stored before the head index that
 * announces it, x86 store order is the barrier the guest's smp_rmb pairs with,
 * and the ring address is guarded by a mutex only to keep a worker from walking
 * it after teardown. Teardown safety is stronger than the net pump's: the
 * workers are joined inside co_arch_boot_loaded before any guest memory is
 * freed, so a worker can never walk a freed page table.
 */

#include <colinux/common/common.h>
#include <colinux/common/libc.h>
#include <colinux/common/debug.h>
#include <colinux/kernel/manager.h>
#include <colinux/kernel/kload.h>
#include <colinux/kernel/cobd.h>
#include <colinux/kernel/cobd_async.h>
#include <colinux/os/kernel/mutex.h>
#include <colinux/os/kernel/wait.h>
#include <colinux/os/kernel/misc.h>

/*
 * Completion ring layout, mirroring struct co_cobd_io in the guest's
 * drivers/block/cobd.c. Declared ABI: the guest struct and these offsets must
 * agree, exactly as net.c's CO_NIO_* mirror struct co_net_io.
 *
 *   enabled    0x00   host writes 1 at start if async is on
 *   comp_head  0x04   host writes (producer)
 *   comp_tail  0x08   guest writes (consumer)
 *   pad        0x0c
 *   comp[]     0x10   stride 0x10, CO_COBD_COMP_SLOTS records of {u32 unit,
 *                     u32 tag, u32 result, u32 pad}
 */
#define CO_CIO_ENABLED		0x00
#define CO_CIO_COMP_HEAD	0x04
#define CO_CIO_COMP_TAIL	0x08
#define CO_CIO_COMP		0x10
#define CO_CIO_COMP_STRIDE	0x10
#define CO_COBD_COMP_SLOTS	128		/* power of two, > 64 max outstanding */

/*
 * The node pool. Every outstanding request holds one node from submission to
 * completion, so the pool must be at least the total possible outstanding:
 * CO_COBD_MAX_UNITS * queue_depth = 4 * 16 = 64. 128 gives headroom; a full
 * pool is the backpressure signal (submit returns PFALSE).
 */
#define CO_COBD_WORK_NODES	128

/*
 * One worker PER UNIT, each with its own queue and its own wake event.
 *
 * A single global worker and a single FIFO was the first cut and it wedged the
 * guest. The backing handle is FILE_SYNCHRONOUS_IO_NONALERT, so requests to one
 * unit serialize no matter how many threads push them -- but with one queue,
 * requests to *different* units serialize too. mke2fs on a fresh 16 GB image
 * makes NTFS zero-fill ahead of each write, which can take minutes, and every
 * read of the root disk behind it in the FIFO waited that long: the guest could
 * not page in its own shell, so it looked frozen even though the kernel was
 * running.
 *
 * The synchronous path had fairness by accident -- one request at a time, so a
 * root-disk read issued between two image writes was serviced immediately. A
 * queue has to be given that property deliberately, and per-unit queues are the
 * cheapest form of it: separate units have separate handles and genuinely
 * proceed in parallel.
 */
#define CO_COBD_WORKERS		CO_COBD_MAX_UNITS

struct co_cobd_work {
	int		    unit;
	unsigned long long  offset;
	unsigned long long  sg_pa;
	unsigned int	    count;
	bool_t		    write;
	unsigned int	    tag;
	struct co_cobd_work* next;
};

static co_manager_t*	    async_manager;
static co_os_mutex_t	    sub_lock;	/* guards the pool free list and the queues */
static co_os_mutex_t	    comp_lock;	/* guards cobd_io_va and the ring producer */
/* One wake channel per unit: a shared SynchronizationEvent would let one
 * worker consume a wakeup meant for another and leave that unit's queue
 * unserved. */
static co_os_wait_t	    sub_wait[CO_COBD_MAX_UNITS];
static unsigned long long   cobd_io_va;
static volatile int	    stopping;
static unsigned long	    async_errors;

static struct co_cobd_work  work_nodes[CO_COBD_WORK_NODES];
static struct co_cobd_work* free_list;	/* stack of unused nodes */
/* Per-unit FIFO, oldest first, so one unit's slow transfer cannot delay
 * another unit's. */
static struct co_cobd_work* fifo_head[CO_COBD_MAX_UNITS];
static struct co_cobd_work* fifo_tail[CO_COBD_MAX_UNITS];

static void* workers[CO_COBD_WORKERS];
static int   worker_count;

co_rc_t co_cobd_async_init(void)
{
	co_rc_t rc;
	int i;

	rc = co_os_mutex_create(&sub_lock);
	if (!CO_OK(rc))
		return rc;

	rc = co_os_mutex_create(&comp_lock);
	if (!CO_OK(rc)) {
		co_os_mutex_destroy(sub_lock);
		sub_lock = NULL;
		return rc;
	}

	for (i = 0; i < CO_COBD_MAX_UNITS; i++) {
		rc = co_os_wait_create(&sub_wait[i]);
		if (!CO_OK(rc)) {
			co_cobd_async_free();
			return rc;
		}
	}

	return CO_RC(OK);
}

void co_cobd_async_free(void)
{
	int i;

	for (i = 0; i < CO_COBD_MAX_UNITS; i++) {
		if (sub_wait[i]) {
			co_os_wait_destroy(sub_wait[i]);
			sub_wait[i] = NULL;
		}
	}
	if (comp_lock) {
		co_os_mutex_destroy(comp_lock);
		comp_lock = NULL;
	}
	if (sub_lock) {
		co_os_mutex_destroy(sub_lock);
		sub_lock = NULL;
	}
}

/*
 * The host producer into the guest completion ring. One record per accepted
 * request. comp_lock serializes multiple workers; the guest is the sole
 * consumer and needs no lock.
 */
static void co_cobd_complete(int unit, unsigned int tag, unsigned int result)
{
	unsigned int head, tail;
	unsigned int rec[4];
	unsigned long long slot;

	co_os_mutex_acquire(comp_lock);

	if (!cobd_io_va) {
		co_os_mutex_release(comp_lock);
		return;			/* torn down: the guest is gone */
	}

	if (!CO_OK(co_kload_read(async_manager, cobd_io_va + CO_CIO_COMP_HEAD,
				 (unsigned char*)&head, sizeof(head))) ||
	    !CO_OK(co_kload_read(async_manager, cobd_io_va + CO_CIO_COMP_TAIL,
				 (unsigned char*)&tail, sizeof(tail)))) {
		co_os_mutex_release(comp_lock);
		return;
	}

	if (head - tail >= CO_COBD_COMP_SLOTS) {
		/* Cannot happen: outstanding <= 64 < slots, and each yields one
		 * completion. If it does, the guest stopped draining -- a bug,
		 * not weather. Drop rather than overwrite an unread record. */
		co_debug_error("cobd async: completion ring full (head %u tail %u)",
			       head, tail);
		co_os_mutex_release(comp_lock);
		return;
	}

	rec[0] = (unsigned int)unit;
	rec[1] = tag;
	rec[2] = result;
	rec[3] = 0;

	slot = cobd_io_va + CO_CIO_COMP +
	       (unsigned long long)(head & (CO_COBD_COMP_SLOTS - 1)) * CO_CIO_COMP_STRIDE;

	/*
	 * The whole record before the index that announces it. x86 does not
	 * reorder stores with each other, so ordinary statement order through
	 * co_kload_write is the barrier the guest's smp_rmb pairs with -- the
	 * identical argument co_net_put relies on.
	 */
	if (!CO_OK(co_kload_write(async_manager, slot,
				  (const unsigned char*)rec, sizeof(rec)))) {
		co_os_mutex_release(comp_lock);
		return;
	}

	head++;
	co_kload_write(async_manager, cobd_io_va + CO_CIO_COMP_HEAD,
		       (const unsigned char*)&head, sizeof(head));

	co_os_mutex_release(comp_lock);
}

static struct co_cobd_work* fifo_pop(int unit)
{
	struct co_cobd_work* node;

	co_os_mutex_acquire(sub_lock);
	node = fifo_head[unit];
	if (node) {
		fifo_head[unit] = node->next;
		if (!fifo_head[unit])
			fifo_tail[unit] = NULL;
	}
	co_os_mutex_release(sub_lock);

	return node;
}

static void node_free(struct co_cobd_work* node)
{
	co_os_mutex_acquire(sub_lock);
	node->next = free_list;
	free_list  = node;
	co_os_mutex_release(sub_lock);
}

static void co_cobd_worker(void* arg)
{
	int unit = (int)(long long)arg;

	for (;;) {
		struct co_cobd_work* node = fifo_pop(unit);
		co_rc_t rc;
		unsigned int result;

		if (!node) {
			if (stopping)
				return;
			co_os_wait_sleep(sub_wait[unit]);
			continue;
		}

		rc = co_cobd_request_sg(async_manager, node->unit, node->offset,
					node->sg_pa, node->count, node->write);
		result = CO_OK(rc) ? 0 : 1;
		if (result)
			async_errors++;		/* one worker today; see header */

		co_cobd_complete(node->unit, node->tag, result);
		node_free(node);
	}
}

co_rc_t co_cobd_async_start(co_manager_t* manager, unsigned long long va)
{
	unsigned int enabled = 1;
	int i;

	async_manager = manager;
	cobd_io_va    = va;
	stopping      = 0;
	async_errors  = 0;
	worker_count  = 0;

	/* (Re)build the free list; every unit's queue starts empty. */
	free_list = NULL;
	for (i = 0; i < CO_COBD_MAX_UNITS; i++)
		fifo_head[i] = fifo_tail[i] = NULL;
	for (i = 0; i < CO_COBD_WORK_NODES; i++) {
		work_nodes[i].next = free_list;
		free_list = &work_nodes[i];
	}

	/* Tell the guest async is live -- it gates the very first submission. */
	if (!CO_OK(co_kload_write(manager, cobd_io_va + CO_CIO_ENABLED,
				  (const unsigned char*)&enabled, sizeof(enabled)))) {
		co_debug_error("cobd async: could not enable the guest ring");
		cobd_io_va = 0;
		return CO_RC(ERROR);
	}

	/* One worker per unit, so a slow transfer on one disk cannot delay
	 * another disk's -- see the CO_COBD_WORKERS comment. */
	for (i = 0; i < CO_COBD_WORKERS; i++) {
		workers[i] = co_os_kthread_start(co_cobd_worker,
						 (void*)(long long)i);
		if (!workers[i])
			break;
		worker_count++;
	}

	if (worker_count == 0) {
		co_debug_error("cobd async: no worker thread started");
		cobd_io_va = 0;
		return CO_RC(ERROR);
	}

	co_debug("cobd async: %d worker(s), ring at 0x%llx", worker_count, va);
	return CO_RC(OK);
}

void co_cobd_async_stop(void)
{
	int i;

	if (worker_count == 0)
		return;

	/*
	 * Ask every worker to leave, then join each. A worker blocked in a
	 * synchronous transfer at this moment delays the join by at most one
	 * request; it must run to completion and not be cancelled (the "unload
	 * the driver first, never taskkill" rule). Joining here, before
	 * co_arch_boot_loaded frees any guest memory, is what makes a worker's
	 * page-table walk safe by construction.
	 *
	 * Each worker has its own wake channel, so every one is woken
	 * individually -- a shared event would let one worker swallow the
	 * wakeup and leave another sleeping through the join forever.
	 */
	stopping = 1;
	for (i = 0; i < worker_count; i++)
		co_os_wait_wakeup(sub_wait[i]);
	for (i = 0; i < worker_count; i++) {
		co_os_kthread_join(workers[i]);
		workers[i] = NULL;
	}
	worker_count = 0;

	/* No worker can produce after this; retire the ring address. */
	co_os_mutex_acquire(comp_lock);
	cobd_io_va = 0;
	co_os_mutex_release(comp_lock);

	/* Drain the queues back to a clean state for the next run. */
	co_os_mutex_acquire(sub_lock);
	for (i = 0; i < CO_COBD_MAX_UNITS; i++)
		fifo_head[i] = fifo_tail[i] = NULL;
	free_list = NULL;
	co_os_mutex_release(sub_lock);
}

bool_t co_cobd_async_submit(int unit, unsigned long long offset,
			    unsigned long long sg_pa, unsigned int count,
			    bool_t write, unsigned int tag)
{
	struct co_cobd_work* node;

	if (unit < 0 || unit >= CO_COBD_MAX_UNITS)
		return PFALSE;

	co_os_mutex_acquire(sub_lock);

	node = free_list;
	if (!node) {
		co_os_mutex_release(sub_lock);
		return PFALSE;			/* pool full: tell the guest busy */
	}
	free_list = node->next;

	node->unit   = unit;
	node->offset = offset;
	node->sg_pa  = sg_pa;
	node->count  = count;
	node->write  = write;
	node->tag    = tag;
	node->next   = NULL;

	if (fifo_tail[unit])
		fifo_tail[unit]->next = node;
	else
		fifo_head[unit] = node;
	fifo_tail[unit] = node;

	co_os_mutex_release(sub_lock);

	co_os_wait_wakeup(sub_wait[unit]);
	return PTRUE;
}

unsigned long co_cobd_async_errors(void)
{
	return async_errors;
}
