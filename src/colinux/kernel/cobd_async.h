/*
 * This source code is a part of coLinux source package.
 *
 * The code is licensed under the GPL. See the COPYING file at
 * the root directory.
 */

#ifndef __COLINUX_KERNEL_COBD_ASYNC_H__
#define __COLINUX_KERNEL_COBD_ASYNC_H__

#include <colinux/common/common.h>
#include <colinux/kernel/manager.h>

/*
 * Asynchronous cooperative block I/O -- defect 3 (doc/async-cobd-design).
 *
 * The synchronous path performs the whole ZwReadFile/ZwWriteFile inline on the
 * CPU-pinned monitor thread, inside the crossing, so the single-CPU guest makes
 * no progress at all for the duration of host disk I/O. This module moves the
 * transfer onto kernel worker threads: the monitor loop copies a submitted
 * request into a queue and returns to the guest immediately, a worker performs
 * the transfer, and the guest reaps the completion off its cooperative tick
 * through the co_colinux_cobd_io ring in guest memory.
 *
 * Lifecycle, all on the monitor thread inside co_arch_boot_loaded:
 *   co_cobd_async_start(manager, cobd_io_va)   before the loop (async on)
 *   co_cobd_async_submit(...)                  from the loop on BLOCK_IO
 *   co_cobd_async_stop()                       after the loop, before any guest
 *                                              memory is freed -- it joins every
 *                                              worker, so no worker can walk a
 *                                              freed guest page table.
 */

extern co_rc_t co_cobd_async_init(void);
extern void    co_cobd_async_free(void);

/*
 * Start/stop the worker pool for one run. start records the guest completion
 * ring address and the manager the workers transfer through, writes enabled=1
 * into the ring, and spawns the workers. stop reverses it and JOINS -- it does
 * not return until every worker has left the driver.
 */
extern co_rc_t co_cobd_async_start(co_manager_t* manager,
				   unsigned long long cobd_io_va);
extern void    co_cobd_async_stop(void);

/*
 * Queue one request. Returns PTRUE if accepted (a worker will perform it and
 * post the completion), PFALSE if the node pool is full -- the caller must then
 * tell the guest "busy" so blk-mq requeues. Never performs I/O itself.
 */
extern bool_t co_cobd_async_submit(int unit, unsigned long long offset,
				   unsigned long long sg_pa,
				   unsigned int count, bool_t write,
				   unsigned int tag);

/* Transfers completed with an error, folded into the heartbeat at report time. */
extern unsigned long co_cobd_async_errors(void);

#endif
