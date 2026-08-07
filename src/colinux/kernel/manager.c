/*
 * This source code is a part of coLinux source package.
 *
 * Dan Aloni <da-x@colinux.org>, 2003-2004 (c)
 *
 * The code is licensed under the GPL. See the COPYING file at
 * the root directory.
 */

/* GCC's own freestanding stddef.h, for size_t as a pointer-sized integer. */
#include <stddef.h>

#include <colinux/common/libc.h>
#include <colinux/common/version.h>
#include <colinux/os/kernel/alloc.h>
#include <colinux/os/kernel/monitor.h>
#include <colinux/os/kernel/manager.h>
#include <colinux/os/kernel/misc.h>
#include <colinux/os/kernel/mutex.h>
#include <colinux/os/timer.h>
#include <colinux/arch/mmu.h>
#include <colinux/arch/probe.h>
#include <colinux/arch/state.h>
#include <colinux/arch/switch.h>
#include <colinux/arch/space.h>

#include "manager.h"
#include "monitor.h"
#include "pages.h"
#include "reversedpfns.h"
#include "kload.h"
#include "cobd.h"
#include "cobd_async.h"
#include "console.h"
#include "net.h"
#include "vgpu.h"

#ifndef min
# define min(a,b) 	((a)<(b)?(a):(b))
#endif

co_manager_t* co_global_manager = NULL;

static void set_hostmem_usage_limit(co_manager_t* manager)
{
	if (manager->hostmem_amount >= 256) {
		/* more than 256MB */
		/* use_limit = host - 64mb */
		manager->hostmem_usage_limit = manager->hostmem_amount - 64;
	} else {
		/* less then 256MB */
		/* use_limit = host * (3/4) */
		manager->hostmem_usage_limit = manager->hostmem_amount*3/4;
	}

	co_debug("machine RAM use limit: %llu MB", manager->hostmem_usage_limit);

	manager->hostmem_usage_limit <<= 20; /* Megify */
}

co_rc_t co_manager_load(co_manager_t *manager)
{
	co_rc_t rc;

	co_memset(manager, 0, sizeof(*manager));

	co_debug_startup();
	/*
	 * Which build of the driver is actually resident.
	 *
	 * Uploading a new linux.sys and reinstalling the service does not
	 * guarantee the new image is the one running: while any handle is still
	 * open, `sc delete` only marks the service for deletion and the loaded
	 * image stays where it is, so the next install quietly reuses it. That
	 * produced an hour of nonsense -- a daemon and a driver built minutes
	 * apart, disagreeing about the layout of the struct they pass to each
	 * other, so every field the daemon printed was read from the wrong
	 * offset and the guest's CR3 came back as 0x5d.
	 *
	 * This line is the first thing in the log after a load. If it does not
	 * match the build that was just deployed, nothing else in the log is
	 * about the code that was just written.
	 */
	co_debug("loaded to host kernel, built " __DATE__ " " __TIME__);

	co_list_init(&manager->opens);
	co_list_init(&manager->monitors);

	rc = co_os_mutex_create(&manager->lock);
	if (!CO_OK(rc))
		return rc;

	rc = co_os_physical_memory_pages(&manager->hostmem_pages);
	if (!CO_OK(rc))
		goto out_err_mutex;

	manager->hostmem_amount = manager->hostmem_pages >> (20-CO_ARCH_PAGE_SHIFT);
	co_debug("machine has %llu MB of RAM", manager->hostmem_amount);

	/*
	 * There used to be a refusal here for machines with more than 4 GB.
	 *
	 * It was an i386 inheritance and it was load-time fatal: the driver
	 * would not start at all. Nothing in the x86-64 path needs it. Physical
	 * addresses are already 64-bit (co_pa_t, and co_pfn_t in
	 * arch/x86_64/mmu.h), guest RAM is allocated with no ceiling
	 * (MmAllocateContiguousMemory against ~0ULL), guest physical equals
	 * host physical, and the guest's tables are four-level -- so a block
	 * above 4 GB needs no special handling anywhere.
	 *
	 * What did need fixing before the check could go were the accounting
	 * fields themselves, which were "unsigned long" and therefore 32-bit
	 * on Windows x64: hostmem_usage_limit is a byte count and shifting
	 * megabytes into it overflowed above 4 GB.
	 */

	set_hostmem_usage_limit(manager);

	rc = co_debug_init(&manager->debug);
	if (!CO_OK(rc))
		goto out_err_mutex;

	manager->state = CO_MANAGER_STATE_INITIALIZED_DEBUG;

	rc = co_manager_arch_init(manager, &manager->archdep);
	if (!CO_OK(rc))
		goto out_err_debug;

	manager->state = CO_MANAGER_STATE_INITIALIZED_ARCH;

	rc = co_os_manager_init(manager, &manager->osdep);
	if (!CO_OK(rc))
		goto out_err_arch;

	manager->state = CO_MANAGER_STATE_INITIALIZED_OSDEP;

	rc = co_console_init();
	if (!CO_OK(rc))
		goto out_err_os;

	rc = co_net_init();
	if (!CO_OK(rc))
		goto out_err_os;

	rc = co_vgpu_init();
	if (!CO_OK(rc))
		goto out_err_os;

	co_os_idle_wake_init();

	rc = co_cobd_async_init();
	if (!CO_OK(rc))
		goto out_err_os;

	/*
	 * The guest-memory teardown lock, before anything can allocate guest
	 * memory. co_kload_free is reachable from three threads and its frees
	 * must happen once; see kload.c.
	 */
	rc = co_kload_init();
	if (!CO_OK(rc))
		goto out_err_os;

	rc = co_manager_alloc_reversed_pfns(manager);
	if (!CO_OK(rc))
		goto out_err_os;

	manager->state = CO_MANAGER_STATE_INITIALIZED;
	return rc;


/* error path */
out_err_os:
	co_os_manager_free(manager->osdep);

out_err_arch:
	co_manager_arch_free(manager->archdep);

out_err_debug:
	co_debug_free(&manager->debug);

out_err_mutex:
	co_os_mutex_destroy(manager->lock);

	manager->state = CO_MANAGER_STATE_NOT_INITIALIZED;
	return rc;
}

void co_manager_unload(co_manager_t* manager)
{
	co_debug("unloaded from host kernel");

	/*
	 * The guest's memory, before anything it depends on goes away.
	 *
	 * Nothing else frees it here. The daemon calls KLOAD_END on its way
	 * out, which covers a run that finishes -- but not one that is killed,
	 * and reload-driver.sh kills the daemon by name every time it swaps the
	 * driver. Windows does not reclaim MmAllocateContiguousMemory when a
	 * driver unloads; that is the driver's job, and this driver was not
	 * doing it. So every reload after a killed run stranded the guest's
	 * whole memory -- 128 MB in four blocks -- until the machine rebooted.
	 *
	 * It does not announce itself as a leak, either. The symptom is that
	 * contiguous allocation starts failing while the host still reports
	 * gigabytes free, because what was lost is not the quantity, it is the
	 * unbroken runs: each stranded block pins a region and the space
	 * between them stops being large enough for the image.
	 *
	 * First, because co_kload_free() destroys an address space through the
	 * arch layer and reads page tables through the manager, both of which
	 * are torn down below.
	 */
	/*
	 * Before anything is freed: stop any monitor loop still running.
	 *
	 * The process that started it may already be gone -- taskkill /f on a
	 * daemon inside the boot ioctl kills the process but cannot interrupt
	 * its thread, which is executing driver code. Freeing the driver's
	 * state underneath that thread is a use-after-free, and the first
	 * thing it touches is the debug system: bugcheck 0xD5, a write into
	 * freed special pool, at the list insert in co_debug_writev.
	 *
	 * The loop checks the flag every crossing, which is microseconds, so
	 * this waits milliseconds in practice. The bound is there because a
	 * loop that never answers must not hang the unload forever -- and if
	 * that ever happens the machine is already lost.
	 */
	if (co_arch_boot_running()) {
		int spins;

		co_debug("unload: a monitor loop is still running -- asking it to stop");
		co_arch_boot_abort();

		for (spins = 0; spins < 1000 && co_arch_boot_running(); spins++)
			co_os_msleep(10);

		if (co_arch_boot_running())
			co_debug_error("unload: the monitor loop did not stop; "
				       "freeing anyway is not survivable, but "
				       "neither is waiting");
		else
			co_debug("unload: the monitor loop stopped");
	}

	/*
	 * The console next, and before co_kload_free: it reaches guest memory
	 * by walking the guest's page tables, and a console client is a
	 * different process that has no idea this is happening. Retiring the
	 * address takes the console lock, so a pump already inside a walk
	 * finishes before the tables go away. The network rings are the same
	 * arrangement and get the same treatment.
	 */
	co_console_set_address(0);
	co_net_set_address(0);

	if (manager->state >= CO_MANAGER_STATE_INITIALIZED)
		co_kload_free(manager);

	/*
	 * And the backing stores, for the same reason: a handle the driver
	 * opened is the driver's to close. Leaving one open holds a write lock
	 * on the image or the partition, so the next run cannot open it and
	 * reports a file in use rather than anything about itself.
	 */
	co_cobd_detach_all();

	if (manager->state >= CO_MANAGER_STATE_INITIALIZED) {
		co_manager_free_reversed_pfns(manager);
		co_os_mutex_destroy(manager->lock);
	}

	if (manager->state >= CO_MANAGER_STATE_INITIALIZED_OSDEP)
		co_os_manager_free(manager->osdep);

	if (manager->state >= CO_MANAGER_STATE_INITIALIZED_ARCH)
		co_manager_arch_free(manager->archdep);

	if (manager->state >= CO_MANAGER_STATE_INITIALIZED_DEBUG)
		co_debug_free(&manager->debug);

	co_cobd_async_free();
	co_vgpu_free();
	co_net_free();
	co_console_free();
	/* Last, because everything above may still call co_kload_free. */
	co_kload_fini();

	manager->state = CO_MANAGER_STATE_NOT_INITIALIZED;
}

co_rc_t co_manager_send(co_manager_t*		manager,
                        co_manager_open_desc_t 	opened,
                        co_message_t*		message)
{
	bool_t  ret;
	co_rc_t rc = CO_RC_OK;

	co_os_mutex_acquire(opened->lock);

	ret = co_os_manager_userspace_try_send_direct(manager, opened, message);
	if (!ret && opened->active) {
		rc = co_message_dup_to_queue(message, &opened->out_queue);

		if (co_queue_size(&opened->out_queue) > CO_QUEUE_COUNT_LIMIT_BEFORE_SLEEP)
			co_debug("queue %d exceed limit with items %ld",
			         message->to, opened->out_queue.items_count);

		while (co_queue_size(&opened->out_queue) > CO_QUEUE_COUNT_LIMIT_BEFORE_SLEEP
		       && opened->active) {
			co_os_mutex_release(opened->lock);
			co_os_msleep(100);
			co_os_mutex_acquire(opened->lock);
		}
	}

	co_os_mutex_release(opened->lock);

	return rc;
}

co_rc_t co_manager_send_eof(co_manager_t* manager, co_manager_open_desc_t opened)
{
	opened->active = PFALSE;

	return co_os_manager_userspace_eof(manager, opened);
}

co_rc_t co_manager_open(co_manager_t* manager, co_manager_open_desc_t* opened_out)
{
	co_manager_open_desc_t opened;
	co_rc_t rc;

	opened = co_os_malloc(sizeof(*opened));
	if (!opened)
		return CO_RC(OUT_OF_MEMORY);

	co_memset(opened, 0, sizeof(*opened));

	rc = co_os_mutex_create(&opened->lock);
	if (!CO_OK(rc))
		return rc;

	rc = co_os_manager_userspace_open(opened);
	if (!CO_OK(rc)) {
		co_os_mutex_destroy(opened->lock);
		co_os_free(opened);
		return rc;
	}

	opened->magic = CO_MANAGER_OPEN_MAGIC;
	opened->monitor = NULL;
	opened->debug_section = NULL;
	opened->ref_count = 1;
	opened->active = PTRUE;

	co_queue_init(&opened->out_queue);

	co_os_mutex_acquire(manager->lock);
	co_list_add_head(&opened->node, &manager->opens);
	manager->num_opens++;
	co_os_mutex_release(manager->lock);

	*opened_out = opened;

	return CO_RC(OK);
}

/*
 * Is this descriptor still alive?
 *
 * Every close path asks before touching anything, because the alternative is
 * what the minidumps show: a descriptor whose pool block has been recycled,
 * read at offset 0x48 for a pointer that is now somebody else's data, handed
 * to ExFreePool, and a 0x50 inside the pool allocator with nothing left to
 * say which path released it twice. A refusal here costs a leaked descriptor
 * and a log line, and keeps the machine and the evidence.
 */
static bool_t co_manager_open_alive(co_manager_open_desc_t opened, const char* where)
{
	if (opened && opened->magic == CO_MANAGER_OPEN_MAGIC)
		return PTRUE;

	co_debug_error("%s: open descriptor %p is not live (magic 0x%llx) -- "
		       "already released, refusing to touch it", where, opened,
		       opened ? opened->magic : 0ULL);
	return PFALSE;
}

static co_rc_t co_manager_close_(co_manager_t*          manager,
				 co_manager_open_desc_t opened)
{
	if (!co_manager_open_alive(opened, "co_manager_close_"))
		return CO_RC(ERROR);

	/*
	 * Cleared before anything is released, so a second arrival is refused
	 * above rather than repeating the frees below.
	 */
	opened->magic = 0;

	co_os_manager_userspace_close(opened);

	if (opened->monitor != NULL) {
		co_monitor_t *mon = opened->monitor;
		opened->monitor = NULL;
		co_monitor_refdown(mon, PFALSE, opened->monitor_owner);
	}

	if (opened->debug_section != NULL) {
		co_debug_fold(&manager->debug, opened->debug_section);
		opened->debug_section = NULL;
	}

	co_os_mutex_acquire(manager->lock);
	co_list_del(&opened->node);
	manager->num_opens--;
	co_os_mutex_release(manager->lock);

	co_os_mutex_destroy(opened->lock);
	co_queue_flush(&opened->out_queue);
	co_debug("close: releasing desc %p", opened);
	co_os_free(opened);

	return CO_RC(OK);
}

/* must be called only when manager->lock is locked */
co_rc_t co_manager_open_ref(co_manager_open_desc_t opened)
{
	co_rc_t rc = CO_RC(OK);

	co_os_mutex_acquire(opened->lock);
	if (opened->ref_count == 0)
		rc = CO_RC(ERROR);
	else
		opened->ref_count++;
	co_os_mutex_release(opened->lock);

	return rc;
}

co_rc_t co_manager_close(co_manager_t *manager, co_manager_open_desc_t opened)
{
	bool_t close;

	if (!co_manager_open_alive(opened, "co_manager_close"))
		return CO_RC(ERROR);

	co_os_mutex_acquire(opened->lock);
	opened->ref_count--;
	close = (opened->ref_count == 0);
	co_os_mutex_release(opened->lock);

	if (close) {
		return co_manager_close_(manager, opened);
	}

	return CO_RC(OK);
}

/*
 * Map the guest's RAM into the calling process, in slices.
 *
 * Called from the ioctl path, so it runs in the requesting process's context
 * -- which is required: MmMapLockedPagesSpecifyCache with UserMode maps into
 * whatever process is current.
 *
 * All or nothing. A half-mapped guest is a caller that thinks it has a
 * complete view and silently reads zeroes off the end of the part that
 * worked, which is the kind of failure that gets diagnosed as a GPU bug three
 * rungs later.
 */
static co_rc_t co_manager_kmap(co_manager_t*		 manager,
			       co_manager_open_desc_t	 opened,
			       co_manager_ioctl_kmap_t*	 params)
{
	unsigned long long slice_bytes = params->max_slice ? params->max_slice
							  : CO_KMAP_SLICE_BYTES;
	int blocks, i, n = 0;

	params->count	    = 0;
	params->total_bytes = 0;

	if (!opened)
		return CO_RC(INVALID_PARAMETER);

	/* One window per handle. A second request would leak the first. */
	if (opened->kmap_slices != 0)
		return CO_RC(ERROR);

	if (slice_bytes < CO_ARCH_PAGE_SIZE || slice_bytes > CO_KMAP_SLICE_BYTES)
		slice_bytes = CO_KMAP_SLICE_BYTES;

	blocks = co_kload_block_count();
	if (blocks == 0)
		return CO_RC(ERROR);

	opened->kmap_slice = co_os_malloc(sizeof(opened->kmap_slice[0]) *
					  CO_KMAP_MAX_RANGES);
	if (!opened->kmap_slice)
		return CO_RC(OUT_OF_MEMORY);
	co_memset(opened->kmap_slice, 0,
		  sizeof(opened->kmap_slice[0]) * CO_KMAP_MAX_RANGES);

	for (i = 0; i < blocks; i++) {
		void*		   va;
		unsigned long long pa, bytes, off;

		if (!CO_OK(co_kload_block(i, &va, &pa, &bytes)))
			continue;

		for (off = 0; off < bytes; off += slice_bytes) {
			unsigned long long this = bytes - off;
			unsigned long	   pages;
			void*		   uva	  = NULL;
			void*		   handle = NULL;

			if (this > slice_bytes)
				this = slice_bytes;
			pages = (unsigned long)(this >> CO_ARCH_PAGE_SHIFT);
			if (pages == 0)
				continue;

			if (n >= CO_KMAP_MAX_RANGES) {
				co_debug("kmap: more than %d slices needed",
					 CO_KMAP_MAX_RANGES);
				goto fail;
			}

			if (!CO_OK(co_os_userspace_map((char*)va + off, pages,
						       &uva, &handle))) {
				co_debug("kmap: slice %d (%lu pages) refused", n, pages);
				goto fail;
			}

			opened->kmap_slice[n].handle  = handle;
			opened->kmap_slice[n].user_va = uva;
			opened->kmap_slice[n].pages   = pages;

			params->range[n].pa	 = pa + off;
			params->range[n].bytes	 = this;
			/*
			 * (size_t), never (unsigned long).
			 *
			 * Windows is LLP64: unsigned long is 32 bits here, so
			 * this cast threw away the top half of every user
			 * address above 4 GB. The daemon then computed
			 * user_va + (gpa - pa) from half a pointer and read it,
			 * which is an access violation on the first touch --
			 * seen as cogpu-daemon.exe dying at +0x75ba on the
			 * 'VGPU' magic, and before that as "the transport page
			 * is not backed".
			 *
			 * It hid for three releases because XP and 7 placed
			 * these MDL mappings below 4 GB, where the truncation
			 * changes nothing. Windows 8.1 places them higher, and
			 * the reported user_va came back as 0xbdc70000 -- a
			 * plausible-looking address that was simply the bottom
			 * 32 bits of the real one.
			 *
			 * The same mistake, with the same cause, is recorded
			 * against this tree's snprintf %p.
			 */
			params->range[n].user_va = (unsigned long long)(size_t)uva;
			params->total_bytes	+= this;
			n++;
			opened->kmap_slices = n;
		}
	}

	if (n == 0)
		goto fail;

	/*
	 * One reference per handle that holds windows, taken after the last
	 * slice rather than per slice: what teardown has to wait for is a
	 * process with a view, not a count of MDLs.
	 */
	co_kload_user_map_get();

	params->count = n;
	/*
	 * max_slice is an input the caller no longer needs; it carries the
	 * driver's own view of the last mapping back out, so the daemon can
	 * print what the kernel saw beside what it sees itself.
	 *
	 * The 0xD0 tag is here because the first attempt at this came back as a
	 * clean zero while params->count, written on the line above, arrived
	 * intact -- which is not a result, it is two indistinguishable failures
	 * wearing the same value. A constant the driver cannot have computed by
	 * accident separates them: if the daemon sees the tag, the write-back
	 * works and co_os_userspace_map is not reaching its own report; if it
	 * sees zero, the field never made the trip and nothing measured through
	 * it has meant anything.
	 */
	{
		extern unsigned long co_last_map_state;
		extern unsigned long co_last_map_protect;

		params->max_slice = 0xD0000000u |
				    ((co_last_map_state & 0xff) << 16) |
				    (co_last_map_protect & 0xffff);
	}
	co_debug("kmap: %d slices, %llu MB mapped into the caller",
		 n, params->total_bytes >> 20);
	return CO_RC(OK);

fail:
	/* Unwind whatever did map; see the all-or-nothing note above. */
	while (n-- > 0) {
		co_os_userspace_unmap(opened->kmap_slice[n].user_va,
				      opened->kmap_slice[n].handle,
				      opened->kmap_slice[n].pages);
	}
	co_os_free(opened->kmap_slice);
	opened->kmap_slice  = NULL;
	opened->kmap_slices = 0;
	params->total_bytes = 0;
	return CO_RC(ERROR);
}

void co_manager_kmap_release(co_manager_t* manager, co_manager_open_desc_t opened)
{
	int i;

	if (!opened || opened->kmap_slices == 0)
		return;

	for (i = 0; i < opened->kmap_slices; i++) {
		if (opened->kmap_slice[i].handle == NULL)
			continue;
		co_os_userspace_unmap(opened->kmap_slice[i].user_va,
				      opened->kmap_slice[i].handle,
				      opened->kmap_slice[i].pages);
	}

	co_debug("kmap: released %d slices", opened->kmap_slices);

	co_os_free(opened->kmap_slice);
	opened->kmap_slice  = NULL;
	opened->kmap_slices = 0;

	/*
	 * After the unmaps, never before: this is what may complete a deferred
	 * teardown and free the very pages just unmapped.
	 */
	co_kload_user_map_put(manager);
}

co_rc_t co_manager_open_desc_deactive_and_close(co_manager_t*	       manager,
						co_manager_open_desc_t opened)
{
	co_rc_t rc;

	if (!co_manager_open_alive(opened, "co_manager_open_desc_deactive_and_close"))
		return CO_RC(ERROR);

	/*
	 * The windows first, while this is still the dying process's context.
	 *
	 * Reached from IRP_MJ_CLEANUP, which is the close path that runs in the
	 * process that is letting go -- and MmUnmapLockedPages for a UserMode
	 * mapping must run there. Doing it later, from the descriptor's final
	 * free, would run in whatever process happened to drop the last
	 * reference.
	 */
	co_manager_kmap_release(manager, opened);

	opened->active = PFALSE;
	if (opened->monitor != NULL) {
		co_monitor_t* mon = opened->monitor;
		int           index;

		co_os_mutex_acquire(mon->connected_modules_write_lock);
		for (index = 0; index < CO_MONITOR_MODULES_COUNT; index++) {
			if (mon->connected_modules[index] != opened)
				continue;

			mon->connected_modules[index] = NULL;

			/*
			 * This can be the last reference, and then `opened` is
			 * gone -- so the loop must not keep comparing against
			 * it and the close below must not run. Both did.
			 */
			if (!CO_OK(co_manager_close(manager, opened)) ||
			    opened->magic != CO_MANAGER_OPEN_MAGIC) {
				co_os_mutex_release(mon->connected_modules_write_lock);
				return CO_RC(OK);
			}
		}
		co_os_mutex_release(mon->connected_modules_write_lock);
	}

	rc = co_manager_close(manager, opened);

	return rc;
}

co_rc_t co_manager_ioctl(co_manager_t* 		manager,
			 unsigned long 		ioctl,
			 void*			io_buffer,
			 unsigned long		in_size,
			 unsigned long		out_size,
			 unsigned long*		return_size,
			 co_manager_open_desc_t opened)
{
	co_rc_t       rc   = CO_RC_OK;
	co_monitor_t* cmon = NULL;

	*return_size = 0;

	switch (ioctl) {
	case CO_MANAGER_IOCTL_STATUS: {
		co_manager_ioctl_status_t* params;
		static const char compile_time[] = {COLINUX_COMPILE_TIME};

		params                        = (typeof(params))(io_buffer);
		params->state                 = manager->state;
		params->monitors_count        = manager->monitors_count;
		params->periphery_api_version = CO_LINUX_PERIPHERY_API_VERSION;
		params->linux_api_version     = CO_LINUX_API_VERSION;

		if (out_size < sizeof(*params)) {
			// Fallback: old daemon ask status
			*return_size = sizeof(*params) - sizeof(params->compile_time);
			return CO_RC(OK);
		}

		co_memcpy(params->compile_time,
			  compile_time,
			  min(sizeof(params->compile_time) - 1,
			      sizeof(compile_time)));

		*return_size = sizeof(*params);
		return CO_RC(OK);
	}

	case CO_MANAGER_IOCTL_PROBE_VA: {
		co_manager_ioctl_probe_va_t* params;

		params = (typeof(params))(io_buffer);

		if (in_size < sizeof(*params) || out_size < sizeof(*params))
			return CO_RC(INVALID_PARAMETER);

		/*
		 * Needs the arch layer initialised, since the walk reads CR3 and maps
		 * page frames through the manager.
		 */
		if (manager->state < CO_MANAGER_STATE_INITIALIZED) {
			params->rc        = CO_RC(ERROR);
			params->supported = PFALSE;
			*return_size      = sizeof(*params);
			return CO_RC(OK);
		}

		co_arch_probe_va(manager, params);

		*return_size = sizeof(*params);
		return CO_RC(OK);
	}

	case CO_MANAGER_IOCTL_PROBE_PASSAGE: {
		co_manager_ioctl_probe_passage_t* params;
		int pages = sizeof(co_arch_passage_page_t) / CO_ARCH_PAGE_SIZE;
		int index;

		params = (typeof(params))(io_buffer);

		if (in_size < sizeof(*params) || out_size < sizeof(*params))
			return CO_RC(INVALID_PARAMETER);

		co_memset(params, 0, sizeof(*params));
		params->pages = pages;

		if (manager->state < CO_MANAGER_STATE_INITIALIZED) {
			params->rc   = CO_RC(ERROR);
			*return_size = sizeof(*params);
			return CO_RC(OK);
		}

		/*
		 * Try each allocator in turn: allocate a passage page's worth, note
		 * where it landed, walk its own mapping, free it. What matters is the
		 * PTE -- specifically NX, and the PWT/PCD caching bits, since the
		 * passage code executes from this page on every world switch.
		 */
		for (index = 0; index < CO_PROBE_ALLOC_MAX; index++) {
			co_probe_alloc_result_t* r = &params->result[index];
			const char* name = NULL;
			void* page;

			if (!co_os_alloc_method(index, &name))
				break;

			params->count = index + 1;
			co_snprintf(r->name, sizeof(r->name), "%s", name ? name : "?");
			r->used = (index == co_os_exec_alloc_index()) ? PTRUE : PFALSE;

			page = co_os_alloc_pages_by(index, pages);
			if (page == NULL) {
				r->ok = PFALSE;
				continue;
			}

			r->ok = PTRUE;
			/* size_t: unsigned long is 32 bits under LLP64 and would
			 * truncate a kernel pointer. */
			r->va = (unsigned long long)(size_t)page;
			r->pa = co_os_virt_to_phys(page);

			r->walk.va = r->va;
			co_arch_probe_va(manager, &r->walk);

			co_os_free_pages_by(index, page, pages);
		}

		params->rc   = CO_RC(OK);
		*return_size = sizeof(*params);
		return CO_RC(OK);
	}

	case CO_MANAGER_IOCTL_SAVE_STATE: {
		co_manager_ioctl_save_state_t* params;

		params = (typeof(params))(io_buffer);

		if (in_size < sizeof(*params) || out_size < sizeof(*params))
			return CO_RC(INVALID_PARAMETER);

		{
			int restore = params->restore;

			co_memset(params, 0, sizeof(*params));
			params->restore = restore;
		}

#if defined(__x86_64__) || defined(CONFIG_X86_64)
		params->supported = PTRUE;
		/*
		 * GDTR, IDTR, TR and GS_BASE are per processor, so capturing
		 * them on one and putting them back on another is meaningless
		 * at best. Pinned even for the read-only case: a state dump
		 * taken across a migration describes no processor that exists.
		 */
		co_os_pin_cpu();
		if (params->restore)
			co_arch_test_save_restore(&params->state);
		else
			co_arch_save_state(&params->state);
		co_os_unpin_cpu();
#else
		params->supported = PFALSE;
#endif
		params->rc   = CO_RC(OK);
		*return_size = sizeof(*params);
		return CO_RC(OK);
	}

	case CO_MANAGER_IOCTL_KLOAD_BEGIN: {
		co_manager_ioctl_kload_begin_t* params = (typeof(params))(io_buffer);

		if (in_size < sizeof(*params) || out_size < sizeof(*params))
			return CO_RC(INVALID_PARAMETER);
		if (manager->state < CO_MANAGER_STATE_INITIALIZED) {
			params->rc = CO_RC(ERROR);
			*return_size = sizeof(*params);
			return CO_RC(OK);
		}

		params->rc = co_kload_begin(manager, params->min_va, params->max_va);
		*return_size = sizeof(*params);
		return CO_RC(OK);
	}

	case CO_MANAGER_IOCTL_KLOAD_CHUNK: {
		co_manager_ioctl_kload_chunk_t* params = (typeof(params))(io_buffer);

		if (in_size < sizeof(*params))
			return CO_RC(INVALID_PARAMETER);
		if (!params->zero && in_size < sizeof(*params) + params->size)
			return CO_RC(INVALID_PARAMETER);

		params->rc = co_kload_chunk(manager, params->va, params->data,
					    params->size, params->zero ? PTRUE : PFALSE);
		*return_size = sizeof(*params);
		return CO_RC(OK);
	}

	case CO_MANAGER_IOCTL_KLOAD_VERIFY: {
		co_manager_ioctl_kload_verify_t* params = (typeof(params))(io_buffer);
		unsigned long long sum = 0;

		if (in_size < sizeof(*params) || out_size < sizeof(*params))
			return CO_RC(INVALID_PARAMETER);

		params->rc     = co_kload_verify(manager, params->va, params->size, &sum);
		params->checksum = sum;
		params->pages  = co_kload_pages();
		params->chunks = co_kload_chunks();
		params->tables = co_kload_space()
				 ? co_arch_guest_space_tables(co_kload_space()) : 0;
		*return_size = sizeof(*params);
		return CO_RC(OK);
	}

	case CO_MANAGER_IOCTL_KREAD: {
		co_manager_ioctl_kread_t* params = (typeof(params))(io_buffer);

		if (in_size < sizeof(*params))
			return CO_RC(INVALID_PARAMETER);
		if (out_size < sizeof(*params) + params->size)
			return CO_RC(INVALID_PARAMETER);

		/*
		 * The locked reader, because this one is reachable from any
		 * process at any moment -- including while another is closing
		 * its handle and tearing the address space down. See
		 * co_kload_read_locked; the size cap is what keeps a reader from
		 * holding a teardown open indefinitely.
		 */
		params->rc = co_kload_read_locked(manager, params->va,
						  params->data, params->size);
		*return_size = sizeof(*params) + params->size;
		return CO_RC(OK);
	}

	case CO_MANAGER_IOCTL_KMAP: {
		co_manager_ioctl_kmap_t* params = (typeof(params))(io_buffer);

		if (in_size < sizeof(*params) || out_size < sizeof(*params))
			return CO_RC(INVALID_PARAMETER);

		params->rc = co_manager_kmap(manager, opened, params);
		*return_size = sizeof(*params);
		return CO_RC(OK);
	}

	case CO_MANAGER_IOCTL_KUNMAP: {
		co_manager_ioctl_kunmap_t* params = (typeof(params))(io_buffer);

		if (in_size < sizeof(*params) || out_size < sizeof(*params))
			return CO_RC(INVALID_PARAMETER);

		params->released = (unsigned long)(opened ? opened->kmap_slices : 0);
		co_manager_kmap_release(manager, opened);
		params->rc = CO_RC(OK);
		*return_size = sizeof(*params);
		return CO_RC(OK);
	}

	case CO_MANAGER_IOCTL_VGPU: {
		co_manager_ioctl_vgpu_t* params = (typeof(params))(io_buffer);

		if (in_size < sizeof(*params) || out_size < sizeof(*params))
			return CO_RC(INVALID_PARAMETER);

		params->va	 = co_vgpu_address();
		params->query_pa = 0;
		params->rc	 = CO_RC(OK);

		/*
		 * One guest virtual address translated, if asked. The daemon's
		 * windows are indexed by physical address, so it needs this
		 * once for the transport structure and never again -- every
		 * descriptor it reads afterwards already carries a physical
		 * address, which is the property this whole design rests on.
		 */
		if (params->query_va) {
			co_pa_t pa = 0;

			if (CO_OK(co_kload_virt_to_phys(manager, params->query_va, &pa)))
				params->query_pa = pa;
			else
				params->rc = CO_RC(NOT_FOUND);
		}

		*return_size = sizeof(*params);
		return CO_RC(OK);
	}

	case CO_MANAGER_IOCTL_VGPU_WAKE: {
		co_manager_ioctl_vgpu_wake_t* params = (typeof(params))(io_buffer);

		if (in_size < sizeof(*params) || out_size < sizeof(*params))
			return CO_RC(INVALID_PARAMETER);

		/*
		 * The daemon has just published completions into guest RAM.
		 * Cut the monitor's idle sleep short so the guest reaps them
		 * now instead of at the end of a backoff tick.
		 *
		 * Every vCPU, because the daemon has no idea which one is
		 * waiting on the fence -- and the ones that are not simply
		 * wake, find nothing and sleep again. Waking the wrong one
		 * costs a re-entry; waking none costs the whole tick this
		 * doorbell exists to avoid.
		 */
		co_os_idle_wake_all();

		params->rc = CO_RC(OK);
		*return_size = sizeof(*params);
		return CO_RC(OK);
	}

	case CO_MANAGER_IOCTL_COBD: {
		co_manager_ioctl_cobd_t* params = (typeof(params))(io_buffer);

		if (in_size < sizeof(*params) || out_size < sizeof(*params))
			return CO_RC(INVALID_PARAMETER);

		/*
		 * The path arrives from userspace, so it is not trusted to be
		 * terminated: co_os_bdev_open hands it to the string
		 * conversion, which walks to the first zero.
		 */
		params->path[sizeof(params->path) - 1] = '\0';
		params->size = 0;
		params->rc   = co_cobd_attach((int)params->unit, params->path,
					      &params->size);
		*return_size = sizeof(*params);
		return CO_RC(OK);
	}

	case CO_MANAGER_IOCTL_KLOAD_ENTER: {
		co_manager_ioctl_test_switch_t* params = (typeof(params))(io_buffer);
		co_arch_switch_test_t result;
		unsigned long long entry_va;
		co_rc_t trc;

		if (in_size < sizeof(*params) || out_size < sizeof(*params))
			return CO_RC(INVALID_PARAMETER);

		/* the entry address travels inwards in code_va */
		entry_va = params->code_va;
		co_memset(params, 0, sizeof(*params));

		co_os_pin_cpu();
		trc = co_arch_enter_loaded(manager, co_kload_space(), entry_va, &result);
		co_os_unpin_cpu();

		params->rc         = trc;
		params->supported  = result.supported;
		params->succeeded  = result.succeeded;
		params->code_va    = result.code_va;
		params->guest_cr3  = result.guest_cr3;
		params->guest_gdt  = result.guest_gdt;
		params->guest_idt  = result.guest_idt;
		params->guest_stubs = result.guest_stubs;
		params->guest_tss  = result.guest_tss;
		params->ist_stack  = result.ist_stack;
		params->guest_text = result.guest_text;
		params->guest_stack = result.guest_stack;
		params->tables     = result.tables;
		params->fault_handler = result.fault_handler;
		params->expected   = result.expected;
		params->observed   = result.observed;
		params->faulted    = result.faulted;
		params->fault_rip  = result.fault_rip;
		params->vector     = result.vector;
		params->error_code = result.error_code;
		params->cr2        = result.cr2;
		params->preflight_checked = result.preflight_checked;
		params->preflight_failed  = result.preflight_failed;
		params->preflight_level   = result.preflight_level;
		params->preflight_va      = result.preflight_va;
		params->passage_va = result.passage_va;
		params->passage_pa = result.passage_pa;
		params->host_cr3   = result.host_cr3;
		params->code_size  = result.code_size;

		*return_size = sizeof(*params);
		return CO_RC(OK);
	}

	case CO_MANAGER_IOCTL_KCALL: {
		co_manager_ioctl_kcall_t* params = (typeof(params))(io_buffer);
		co_arch_kcall_test_t result;
		unsigned long long memset_va, strlen_va, snprintf_va;
		unsigned long long epk_va, ec_va, cc_va, ring_va;
		co_rc_t trc;

		if (in_size < sizeof(*params) || out_size < sizeof(*params))
			return CO_RC(INVALID_PARAMETER);

		memset_va = params->memset_va;
		strlen_va = params->strlen_va;
		snprintf_va = params->snprintf_va;
		epk_va  = params->early_printk_va;
		ec_va   = params->early_console_va;
		cc_va   = params->colinux_console_va;
		ring_va = params->ring_symbol_va;
		co_memset(params, 0, sizeof(*params));

		co_os_pin_cpu();
		trc = co_arch_test_kernel_code(manager, co_kload_space(),
					       memset_va, strlen_va, snprintf_va,
					       epk_va, ec_va, cc_va, ring_va, &result);
		co_os_unpin_cpu();

		params->rc		= trc;
		params->supported	= result.supported;
		params->succeeded	= result.succeeded;
		params->memset_va	= result.memset_va;
		params->strlen_va	= result.strlen_va;
		params->scratch_va	= result.scratch_va;
		params->memset_ret	= result.memset_ret;
		params->strlen_ret	= result.strlen_ret;
		params->strlen_expected = result.strlen_expected;
		params->snprintf_va	= result.snprintf_va;
		params->snprintf_ret	= result.snprintf_ret;
		params->snprintf_expected = result.snprintf_expected;
		params->pattern_ok	= result.pattern_ok;
		params->text_ok		= result.text_ok;
		params->console_ring_va   = result.console_ring_va;
		params->console_written   = result.console_written;
		params->console_capacity  = result.console_capacity;
		params->console_ok	  = result.console_ok;
		co_memcpy(params->console_text, result.console_text, sizeof(params->console_text));
		co_memcpy(params->text, result.text, sizeof(params->text));
		params->first_bad	= result.first_bad;
		params->first_bad_byte	= result.first_bad_byte;
		params->faulted		= result.faulted;
		params->vector		= result.vector;
		params->fault_rip	= result.fault_rip;
		params->cr2		= result.cr2;

		*return_size = sizeof(*params);
		return CO_RC(OK);
	}

	case CO_MANAGER_IOCTL_KRAM: {
		co_manager_ioctl_kram_t* params = (typeof(params))(io_buffer);
		unsigned long long ram, text, end;

		if (in_size < sizeof(*params) || out_size < sizeof(*params))
			return CO_RC(INVALID_PARAMETER);

		ram = params->ram_bytes; text = params->text_va; end = params->end_va;
		co_memset(params, 0, sizeof(*params));

		params->rc = co_kload_build_ram(manager, ram, text, end);
		params->ram_pages    = co_kload_ram_pages();
		params->phys_base    = co_kload_phys_base();
		params->range_count  = co_kload_range_count();
		if (params->range_count > CO_KRAM_MAX_RANGES)
			params->range_count = CO_KRAM_MAX_RANGES;
		{
			int i;

			for (i = 0; i < params->range_count; i++) {
				co_kload_range(i, &params->range[i].pa,
					       &params->range[i].usable,
					       &params->range[i].reserved);
				params->total_usable += params->range[i].usable;
			}
		}
		params->tables = co_kload_space()
				 ? co_arch_guest_space_tables(co_kload_space()) : 0;

		*return_size = sizeof(*params);
		return CO_RC(OK);
	}

	case CO_MANAGER_IOCTL_CONSOLE: {
		co_manager_ioctl_console_t* params = (typeof(params))(io_buffer);
		unsigned long offered, wanted;

		if (in_size < sizeof(*params) || out_size < sizeof(*params))
			return CO_RC(INVALID_PARAMETER);

		*return_size = sizeof(*params);

		offered = params->in_size;
		if (offered > sizeof(params->in))
			offered = sizeof(params->in);

		/*
		 * The caller's output size is honoured, clamped to the buffer.
		 *
		 * It used to pass sizeof(params->out) unconditionally, which
		 * throws away what the caller asked for -- and a caller asking
		 * for nothing means it. The console server probes for a live
		 * guest with a zero-length call precisely so it can find out
		 * whether a run has ended without consuming anything, because
		 * console output pulled out of the ring is gone: it is not
		 * buffered anywhere else and the next client never sees it.
		 *
		 * Ignoring the zero turned that probe into a drain. The server
		 * runs it every 200 ms whenever no client is attached, so
		 * between one terminal session and the next it was quietly
		 * eating everything the guest printed. What that looks like
		 * from outside is a guest that has gone unresponsive -- output
		 * missing, commands apparently ignored -- when the guest is
		 * perfectly healthy and the console is deleting its words.
		 */
		wanted = params->out_size;
		if (wanted > sizeof(params->out))
			wanted = sizeof(params->out);

		params->rc = co_console_pump(manager,
					     params->in, offered, &params->in_taken,
					     params->out, wanted,
					     &params->out_len);
		return CO_RC(OK);
	}

	case CO_MANAGER_IOCTL_KBOOT: {
		co_manager_ioctl_kboot_t* params = (typeof(params))(io_buffer);
		co_arch_boot_t in;
		co_arch_boot_result_t result;
		co_rc_t trc;

		if (in_size < sizeof(*params) || out_size < sizeof(*params))
			return CO_RC(INVALID_PARAMETER);

		in.entry_va           = params->entry_va;
		in.initial_code_va    = params->initial_code_va;
		in.start_kernel_va    = params->start_kernel_va;
		in.early_console_va   = params->early_console_va;
		in.colinux_console_va = params->colinux_console_va;
		in.ring_symbol_va     = params->ring_symbol_va;
		in.guest_flag_va      = params->guest_flag_va;
		in.tick_entry_va      = params->tick_entry_va;
		in.virtual_if_va      = params->virtual_if_va;
		in.step               = params->step;
		in.batch              = params->batch;
		in.kernel_table_count = params->kernel_table_count;
		co_memcpy(in.kernel_tables, params->kernel_tables,
			  sizeof(in.kernel_tables));
		in.ex_table_start = params->ex_table_start;
		in.ex_table_stop  = params->ex_table_stop;
		in.passage_symbol_va  = params->passage_symbol_va;
		co_console_set_address(params->console_io_va);
		co_net_set_address(params->net_io_va);
		/*
		 * And the GPU transport, published here for the same reason as
		 * the two above: this is the moment the guest's address space
		 * is built and its symbols are known, and the daemons that read
		 * these structures are separate processes that cannot be told
		 * any earlier. Zero when the kernel has no transport.
		 */
		co_vgpu_set_address(manager, params->vgpu_io_va);
		in.cobd_io_va         = params->cobd_io_va;
		in.async_cobd         = params->async_cobd;
		in.max_switches       = params->max_switches ? params->max_switches : 4096;

		co_memset(params, 0, sizeof(*params));

		/*
		 * Pinned for the whole call, not just the monitor loop.
		 *
		 * co_arch_boot_loaded captures the host's GDTR, IDTR, TR and
		 * GS_BASE before it enters the guest and restores them on every
		 * crossing back, and all of those are per processor. The capture
		 * has to happen on the processor the switches will run on, so
		 * the pin has to come first.
		 */
		co_os_pin_cpu();
		trc = co_arch_boot_loaded(manager, co_kload_space(), &in, &result);
		co_os_unpin_cpu();

		/*
		 * The guest said it was finished, so let go of what it was
		 * using -- as much of it as can be let go of here.
		 *
		 * Not its memory: the daemon reads the kernel's printk ring
		 * out of it after this call returns, and that log is the most
		 * useful thing a run produces. That waits for KLOAD_END.
		 *
		 * But the backing files can go now, and should. They were
		 * being held until the driver unloaded, which meant a finished
		 * guest still had its disk image open: the next deploy could
		 * not overwrite root.img, and the fix was to reload the driver
		 * for no other reason. A shut-down guest is not using its
		 * disks. Neither is it using its console, so that is retired
		 * too, which also stops a terminal client walking page tables
		 * belonging to a guest that has stopped.
		 */
		if (result.terminated) {
			co_debug("boot: the guest shut down (reason %llu) -- "
				 "closing its disks and retiring its console",
				 result.terminate_reason);
			co_console_set_address(0);
			co_net_set_address(0);
			co_cobd_detach_all();
		}

		params->rc		 = trc;
		params->supported	 = result.supported;
		params->entry_va	 = result.entry_va;
		params->guest_cr3	 = result.guest_cr3;
		params->tables		 = result.tables;
		params->console_ring_va	 = result.console_ring_va;
		params->console_written	 = result.console_written;
		params->console_capacity = result.console_capacity;
		co_memcpy(params->console_text, result.console_text, sizeof(params->console_text));
		params->faulted		 = result.faulted;
		params->returned_voluntarily = result.returned_voluntarily;
		params->hit_limit	 = result.hit_limit;
		params->hit_deadline	 = result.hit_deadline;
		params->guest_switches	 = result.guest_switches;
		params->unforwardable	 = result.unforwardable;
		params->vmx_present	 = result.vmx_present;
		params->switches	 = result.switches;
		params->interrupts	 = result.interrupts;
		params->ticks_injected	 = result.ticks_injected;
		params->steps		 = result.steps;
		params->trace_next	 = result.trace_next;
		co_memcpy(params->trace, result.trace, sizeof(params->trace));
		params->vector		 = result.vector;
		params->fault_rip	 = result.fault_rip;
		params->error_code	 = result.error_code;
		params->cr2		 = result.cr2;
		params->fault_rdi	 = result.fault_rdi;
		params->fault_rax	 = result.fault_rax;
		params->warnings	 = result.warnings;
		co_memcpy(params->warning_rip, result.warning_rip,
			  sizeof(params->warning_rip));
		params->fixups		 = result.fixups;
		params->fault_extype	 = result.fault_extype;
		params->reached_idle	 = result.reached_idle;
		params->run_yields	 = result.run_yields;
		params->idle_yields	 = result.idle_yields;
		params->block_requests	 = result.block_requests;
		params->block_errors	 = result.block_errors;
		params->terminated	 = result.terminated;
		params->terminate_reason = result.terminate_reason;
		params->stop_operation	 = result.stop_operation;
		co_memcpy(params->stop_regs, result.stop_regs,
			  sizeof(params->stop_regs));
		co_memcpy(params->fixup_rip, result.fixup_rip,
			  sizeof(params->fixup_rip));
		co_memcpy(params->fixup_type, result.fixup_type,
			  sizeof(params->fixup_type));
		params->preflight_checked = result.preflight_checked;
		params->preflight_failed  = result.preflight_failed;
		params->preflight_level	  = result.preflight_level;
		params->preflight_va	  = result.preflight_va;
		params->host_corrupt_field    = result.host_corrupt_field;
		params->host_corrupt_step     = result.host_corrupt_step;
		params->host_corrupt_expected = result.host_corrupt_expected;
		params->host_corrupt_actual   = result.host_corrupt_actual;

		*return_size = sizeof(*params);
		return CO_RC(OK);
	}

	case CO_MANAGER_IOCTL_CONET_DUMP: {
		co_manager_ioctl_conet_dump_t* params = (typeof(params))(io_buffer);

		if (in_size < sizeof(*params))
			return CO_RC(INVALID_PARAMETER);
		if (params->size > CO_CONET_DUMP_MAX ||
		    out_size < sizeof(*params) + params->size)
			return CO_RC(INVALID_PARAMETER);

		params->rc = co_net_dump(manager,
					 &params->tx_head, &params->tx_tail,
					 &params->rx_head, &params->rx_tail,
					 params->start, params->data,
					 &params->size);
		*return_size = sizeof(*params) + params->size;
		return CO_RC(OK);
	}

	case CO_MANAGER_IOCTL_CONET_PUT: {
		co_manager_ioctl_conet_put_t* params = (typeof(params))(io_buffer);

		if (in_size < sizeof(*params))
			return CO_RC(INVALID_PARAMETER);
		/*
		 * The batch is bounded here, before anything reads it: the
		 * declared byte count must fit in the buffer the I/O manager
		 * actually copied, and the frame count must be possible for
		 * that many bytes -- the smallest record is eight bytes, a
		 * length word plus a padded minimum frame. Both come from
		 * userspace, and co_net_put walks the records itself, so a
		 * count that outruns the data would have it reading past the
		 * end of the staging buffer.
		 */
		if (params->size > CO_CONET_PUT_BATCH ||
		    in_size < sizeof(*params) + params->size)
			return CO_RC(INVALID_PARAMETER);
		if (params->frames == 0 || params->frames > params->size / 8)
			return CO_RC(INVALID_PARAMETER);
		if (out_size < sizeof(*params))
			return CO_RC(INVALID_PARAMETER);

		params->rc = co_net_put(manager, params->data, params->size,
					params->frames, &params->taken);
		*return_size = sizeof(*params);
		return CO_RC(OK);
	}

	case CO_MANAGER_IOCTL_CONET_TAKE: {
		co_manager_ioctl_conet_take_t* params = (typeof(params))(io_buffer);

		if (in_size < sizeof(*params) || out_size < sizeof(*params))
			return CO_RC(INVALID_PARAMETER);

		params->rc = co_net_take(manager, params->new_tail,
					 &params->tx_head, &params->tx_tail,
					 &params->rx_head, &params->rx_tail);
		*return_size = sizeof(*params);
		return CO_RC(OK);
	}

	case CO_MANAGER_IOCTL_KSTOP: {
		co_manager_ioctl_kstop_t* params = (typeof(params))(io_buffer);

		if (in_size < sizeof(*params) || out_size < sizeof(*params))
			return CO_RC(INVALID_PARAMETER);

		/*
		 * One flag write, checked by the loop on every crossing --
		 * the mechanism driver unload already trusts. Nothing is
		 * freed or touched here: the run ends through its own exit
		 * path, in the process that owns it. Unsynchronised on
		 * purpose; the flag is monotonic within a run and the reader
		 * is volatile.
		 */
		params->was_running = co_arch_boot_running();
		if (params->was_running)
			co_arch_boot_abort();

		params->rc   = CO_RC(OK);
		*return_size = sizeof(*params);
		return CO_RC(OK);
	}

	case CO_MANAGER_IOCTL_KLOAD_END: {
		/*
		 * Refuse while a monitor loop is running.
		 *
		 * This handler frees the guest's RAM and address space, and it
		 * is reachable from ANY handle -- so today it is only safe
		 * because the one process that sends it is the same one running
		 * the guest, and it sends it after the run ends. That is a
		 * convention held by one program, not a property of the
		 * interface, and R4 puts a second and third process on manager
		 * handles. KSTOP already reads this flag for the same reason;
		 * this makes the convention an invariant for the cost of an if.
		 */
		if (co_arch_boot_running()) {
			co_debug_error("KLOAD_END refused: a monitor loop is "
				       "still running");
			*return_size = 0;
			return CO_RC(ERROR);
		}

		/*
		 * Retire the console before the address space it reads through
		 * is freed. A console client is another process entirely and
		 * would otherwise keep walking page tables that have been
		 * returned to the host -- which is a use-after-free in a
		 * driver, so it bugchecks rather than fails. The network rings
		 * are read the same way by --net-dump and retire the same way.
		 */
		co_console_set_address(0);
		co_net_set_address(0);
		co_cobd_detach_all();
		co_kload_free(manager);
		*return_size = 0;
		return CO_RC(OK);
	}

	case CO_MANAGER_IOCTL_TEST_SPACE: {
		co_manager_ioctl_test_space_t* params;
		co_arch_space_test_t result;
		co_rc_t trc;

		params = (typeof(params))(io_buffer);

		if (in_size < sizeof(*params) || out_size < sizeof(*params))
			return CO_RC(INVALID_PARAMETER);

		co_memset(params, 0, sizeof(*params));

		if (manager->state < CO_MANAGER_STATE_INITIALIZED) {
			params->rc   = CO_RC(ERROR);
			*return_size = sizeof(*params);
			return CO_RC(OK);
		}

		trc = co_arch_test_space(manager, &result);

		params->rc		 = trc;
		params->supported	 = result.supported;
		params->succeeded	 = result.succeeded;
		params->root		 = result.root;
		params->mapped		 = result.mapped;
		params->verified	 = result.verified;
		params->mismatched	 = result.mismatched;
		params->tables		 = result.tables;
		params->first_bad_va	 = result.first_bad_va;
		params->first_bad_expect = result.first_bad_expect;
		params->first_bad_got	 = result.first_bad_got;
		params->unmapped_reported = result.unmapped_reported;
		params->unmapped_level	 = result.unmapped_level;

		*return_size = sizeof(*params);
		return CO_RC(OK);
	}

	case CO_MANAGER_IOCTL_TEST_FAULT:
	case CO_MANAGER_IOCTL_TEST_PAGEFAULT:
	case CO_MANAGER_IOCTL_TEST_BADSTACK:
	case CO_MANAGER_IOCTL_TEST_GUEST:
	case CO_MANAGER_IOCTL_TEST_GUESTFAULT:
	case CO_MANAGER_IOCTL_TEST_RESUME:
	case CO_MANAGER_IOCTL_TEST_ROUNDTRIP:
	case CO_MANAGER_IOCTL_TEST_SWITCH: {
		co_manager_ioctl_test_switch_t* params;
		co_arch_switch_test_t result;
		co_rc_t trc;
		int req_iterations;

		params = (typeof(params))(io_buffer);

		if (in_size < sizeof(*params) || out_size < sizeof(*params))
			return CO_RC(INVALID_PARAMETER);

		/*
		 * iterations is the one field that travels inwards, so it has to be
		 * read before the struct is cleared for the reply.
		 */
		req_iterations = params->iterations;

		co_memset(params, 0, sizeof(*params));

		if (manager->state < CO_MANAGER_STATE_INITIALIZED) {
			params->rc   = CO_RC(ERROR);
			*return_size = sizeof(*params);
			return CO_RC(OK);
		}

		/*
		 * Every one of these crosses into a guest address space and
		 * back, which means every one of them saves and restores per
		 * processor state. They are short enough that a migration is
		 * unlikely rather than impossible, which is exactly the kind of
		 * bug that passes a hundred runs and then takes the machine
		 * down once.
		 */
		co_os_pin_cpu();

		if (ioctl == CO_MANAGER_IOCTL_TEST_ROUNDTRIP)
			trc = co_arch_test_roundtrip(manager, &result, 0);
		else if (ioctl == CO_MANAGER_IOCTL_TEST_FAULT)
			trc = co_arch_test_roundtrip(manager, &result, 1);
		else if (ioctl == CO_MANAGER_IOCTL_TEST_PAGEFAULT)
			trc = co_arch_test_roundtrip(manager, &result, 2);
		else if (ioctl == CO_MANAGER_IOCTL_TEST_BADSTACK)
			trc = co_arch_test_roundtrip(manager, &result, 3);
		else if (ioctl == CO_MANAGER_IOCTL_TEST_GUEST)
			trc = co_arch_test_extern_guest(manager, &result, PFALSE);
		else if (ioctl == CO_MANAGER_IOCTL_TEST_GUESTFAULT)
			trc = co_arch_test_extern_guest(manager, &result, PTRUE);
		else if (ioctl == CO_MANAGER_IOCTL_TEST_RESUME)
			trc = co_arch_test_resume(manager, &result, req_iterations);
		else
			trc = co_arch_test_switch(manager, &result);

		co_os_unpin_cpu();

		params->rc         = trc;
		params->supported  = result.supported;
		params->succeeded  = result.succeeded;
		params->passage_va = result.passage_va;
		params->passage_pa = result.passage_pa;
		params->code_va    = result.code_va;
		params->host_cr3   = result.host_cr3;
		params->guest_cr3  = result.guest_cr3;
		params->expected   = result.expected;
		params->observed   = result.observed;
		params->guest_gdt  = result.guest_gdt;
		params->iterations = result.iterations;
		params->counter    = result.counter;
		params->reg_accum  = result.reg_accum;
		params->guest_stubs = result.guest_stubs;
		params->guest_tss   = result.guest_tss;
		params->ist_stack   = result.ist_stack;
		params->guest_text  = result.guest_text;
		params->guest_stack = result.guest_stack;
		params->tables      = result.tables;
		params->vector      = result.vector;
		params->error_code  = result.error_code;
		params->cr2         = result.cr2;
		params->preflight_checked = result.preflight_checked;
		params->preflight_failed  = result.preflight_failed;
		params->preflight_level   = result.preflight_level;
		params->preflight_va      = result.preflight_va;
		params->guest_idt  = result.guest_idt;
		params->fault_handler = result.fault_handler;
		params->fault_rip  = result.fault_rip;
		params->faulted    = result.faulted;
		params->code_size  = result.code_size;

		*return_size = sizeof(*params);
		return CO_RC(OK);
	}

	case CO_MANAGER_IOCTL_TEST_SMP: {
		co_manager_ioctl_test_smp_t* params;
		co_arch_smp_test_t result;
		co_rc_t trc;
		int req_lane;
		long long req_iterations;

		params = (typeof(params))(io_buffer);

		if (in_size < sizeof(*params) || out_size < sizeof(*params))
			return CO_RC(INVALID_PARAMETER);

		/* input fields are not output fields: read before the clear */
		req_lane       = params->lane;
		req_iterations = params->iterations;

		co_memset(params, 0, sizeof(*params));
		params->lane = req_lane;

		if (manager->state < CO_MANAGER_STATE_INITIALIZED) {
			params->rc   = CO_RC(ERROR);
			*return_size = sizeof(*params);
			return CO_RC(OK);
		}

		if (req_lane < 0 ||
		    (unsigned long)req_lane >= co_os_cpu_count()) {
			co_debug_error("TEST_SMP: lane %d does not name an "
				       "active processor (%lu active)",
				       req_lane, co_os_cpu_count());
			params->rc   = CO_RC(INVALID_PARAMETER);
			*return_size = sizeof(*params);
			return CO_RC(OK);
		}

		/*
		 * Pinned to the *named* processor, not the current one --
		 * distinctness of the lanes is the whole experiment. The
		 * refusal above means a failure here is a race with hot
		 * remove, which the host does not do; report it anyway.
		 */
		if (!co_os_pin_cpu_to((unsigned long)req_lane)) {
			params->rc   = CO_RC(ERROR);
			*return_size = sizeof(*params);
			return CO_RC(OK);
		}

		/*
		 * A private address space: --test-smp is meant to run with no
		 * guest loaded, so there is nothing to join.
		 */
		trc = co_arch_test_smp_lane(manager, &result, req_lane,
					    req_iterations, 0);

		co_os_unpin_cpu();

		params->rc               = trc;
		params->supported        = result.supported;
		params->succeeded        = result.succeeded;
		params->host_cpu         = result.host_cpu;
		params->iterations       = result.iterations;
		params->completed        = result.completed;
		params->interrupts       = result.interrupts;
		params->counter          = result.counter;
		params->reg_accum        = result.reg_accum;
		params->faulted          = result.faulted;
		params->vector           = result.vector;
		params->error_code       = result.error_code;
		params->fault_rip        = result.fault_rip;
		params->unforwardable    = result.unforwardable;
		params->aborted          = result.aborted;
		params->migrated         = result.migrated;
		params->msr_ok           = result.msr_ok;
		params->msr_bad          = result.msr_bad;
		params->msr_want         = result.msr_want;
		params->msr_got          = result.msr_got;
		params->preflight_failed = result.preflight_failed;
		params->preflight_level  = result.preflight_level;
		params->preflight_va     = result.preflight_va;

		*return_size = sizeof(*params);
		return CO_RC(OK);
	}

	case CO_MANAGER_IOCTL_KVCPU_RUN: {
		co_manager_ioctl_kvcpu_run_t* params;
		co_arch_smp_test_t result;
		co_rc_t trc;
		int req_vcpu;
		long long req_iterations;
		unsigned long cores, cpu, chosen;

		params = (typeof(params))(io_buffer);

		if (in_size < sizeof(*params) || out_size < sizeof(*params))
			return CO_RC(INVALID_PARAMETER);

		req_vcpu       = params->vcpu;
		req_iterations = params->iterations;

		co_memset(params, 0, sizeof(*params));
		params->vcpu = req_vcpu;

		if (manager->state < CO_MANAGER_STATE_INITIALIZED) {
			params->rc   = CO_RC(ERROR);
			*return_size = sizeof(*params);
			return CO_RC(OK);
		}

		/*
		 * vCPU 0 is the boot processor and belongs to the boot loop.
		 * A secondary is 1 and up.
		 */
		if (req_vcpu < 1 || req_vcpu >= CO_MAX_VCPUS) {
			params->rc   = CO_RC(INVALID_PARAMETER);
			*return_size = sizeof(*params);
			return CO_RC(OK);
		}

		/*
		 * A core nobody else is holding. Not "the one I am on" and not
		 * "the one whose number matches", because the boot processor
		 * took whichever core the scheduler had it on and the answer
		 * has to work while that run is in progress.
		 */
		cores  = co_os_cpu_count();
		chosen = cores;
		for (cpu = 0; cpu < cores; cpu++) {
			if (!co_arch_vcpu_core_taken(cpu)) {
				chosen = cpu;
				break;
			}
		}

		if (chosen >= cores || !co_os_pin_cpu_to(chosen)) {
			co_debug_error("KVCPU_RUN: no free host processor for vcpu %d "
				       "(%lu cores, all carrying a vCPU)",
				       req_vcpu, cores);
			params->no_free_core = PTRUE;
			params->rc           = CO_RC(ERROR);
			*return_size         = sizeof(*params);
			return CO_RC(OK);
		}

		co_debug("KVCPU_RUN: vcpu %d on host processor %lu of %lu",
			 req_vcpu, chosen, cores);

		/*
		 * Into the running guest's own address space. That is the
		 * point of this ioctl as against a --test-smp lane: one CR3,
		 * two processors.
		 */
		trc = co_arch_test_smp_lane(manager, &result, req_vcpu,
					    req_iterations, 1);

		co_os_unpin_cpu();

		params->rc            = trc;
		params->supported     = result.supported;
		params->succeeded     = result.succeeded;
		params->host_cpu      = result.host_cpu;
		params->iterations    = result.iterations;
		params->completed     = result.completed;
		params->interrupts    = result.interrupts;
		params->counter       = result.counter;
		params->reg_accum     = result.reg_accum;
		params->faulted       = result.faulted;
		params->vector        = result.vector;
		params->error_code    = result.error_code;
		params->fault_rip     = result.fault_rip;
		params->unforwardable = result.unforwardable;
		params->aborted       = result.aborted;
		params->migrated      = result.migrated;
		params->msr_ok        = result.msr_ok;
		params->msr_bad       = result.msr_bad;
		params->msr_want      = result.msr_want;
		params->msr_got       = result.msr_got;

		*return_size = sizeof(*params);
		return CO_RC(OK);
	}

	case CO_MANAGER_IOCTL_INFO: {
		co_manager_ioctl_info_t* params;

		params = (typeof(params))(io_buffer);
		params->hostmem_usage_limit = manager->hostmem_usage_limit;
		params->hostmem_used        = manager->hostmem_used;

		*return_size = sizeof(*params);
		return CO_RC(OK);
	}

	case CO_MANAGER_IOCTL_DEBUG: {
		co_debug_write_vector_t vec;

		vec.vec_size = 0;
		vec.size     = in_size;
		vec.ptr      = io_buffer;

		co_debug_write_log(&manager->debug, &opened->debug_section, &vec, 1);

		return CO_RC(OK);
	}
	case CO_MANAGER_IOCTL_DEBUG_READER: {
		co_manager_ioctl_debug_reader_t *params;

		params = (typeof(params))(io_buffer);
		params->rc = co_debug_read(&manager->debug,
					   params->user_buffer,
					   params->user_buffer_size,
					   &params->filled);

		*return_size = sizeof(*params);

		return CO_RC(OK);
	}
#ifdef COLINUX_DEBUG
	case CO_MANAGER_IOCTL_DEBUG_LEVELS: {
		co_manager_ioctl_debug_levels_t *params;

		params = (typeof(params))(io_buffer);
		if (params->modify) {
			co_global_debug_levels = params->levels;
		} else {
			params->levels = co_global_debug_levels;
		}

		*return_size = sizeof(*params);

		return CO_RC(OK);
	}
#endif
	default:
		break;
	}

	if (manager->state < CO_MANAGER_STATE_INITIALIZED) {
		return CO_RC_ERROR;
	}

	switch (ioctl) {
	case CO_MANAGER_IOCTL_CREATE: {
		co_manager_ioctl_create_t* params = (typeof(params))(io_buffer);

		if (opened->monitor)
			return CO_RC(ERROR);

		rc = co_monitor_create(manager, params, &cmon);
		if (CO_OK(rc)) {
			opened->monitor       = cmon;
			opened->monitor_owner = PTRUE;
		}

		params->rc   = rc;
		*return_size = sizeof(*params);
		break;
	}
	case CO_MANAGER_IOCTL_MONITOR_LIST: {
		co_manager_ioctl_monitor_list_t* params  = (typeof(params))(io_buffer);
		co_monitor_t*                    monitor = NULL;
		co_rc_t                          rc      = CO_RC(OK);
		int                              i       = 0;

		co_os_mutex_acquire(manager->lock);
		co_list_each_entry(monitor, &manager->monitors, node) {
			if (i >= CO_MAX_MONITORS) {
				/* We don't enforce a limit on create, so just
				 * break from the loop and return the first ones
				 */
				break;
			}
			params->ids[i++] = monitor->id;
		}
		co_os_mutex_release(manager->lock);
		params->count = i;

		params->rc   = rc;
		*return_size = sizeof(*params);
		break;
	}
	case CO_MANAGER_IOCTL_ATTACH: {
		co_manager_ioctl_attach_t* params  = (typeof(params))(io_buffer);
		co_monitor_t*              monitor = NULL;
		co_rc_t			   rc      = CO_RC(ERROR);

		co_os_mutex_acquire(manager->lock);
		co_list_each_entry(monitor, &manager->monitors, node) {
			if (monitor->id == params->id) {
				monitor->refcount++;
				opened->monitor = monitor;
				rc = CO_RC(OK);
				break;
			}
		}
		co_os_mutex_release(manager->lock);

		if (!CO_OK(rc)) {
			opened->monitor = NULL;
		}

		if (opened->monitor) {
			int                    index;
			co_module_t            module;
			co_manager_open_desc_t old_opened;

			cmon = opened->monitor;

			co_os_mutex_acquire(cmon->connected_modules_write_lock);

			for (index = 0; index < params->num_modules; index++) {
				module = params->modules[index];
				old_opened = cmon->connected_modules[module];
				if (old_opened)
					co_manager_close(manager, old_opened);
				cmon->connected_modules[module] = opened;
				opened->ref_count++;
			}

			co_os_mutex_release(cmon->connected_modules_write_lock);
		}

		*return_size = sizeof(*params);
		params->rc = rc;
		break;
	}
	case CO_MANAGER_IOCTL_MONITOR: {
		co_manager_ioctl_monitor_t* params = (typeof(params))(io_buffer);

		*return_size = sizeof(*params);

		if (in_size < sizeof(*params)) {
			co_debug_error("monitor ioctl too small! (%lu < %llu)",
			               in_size, (unsigned long long)sizeof(*params));
			params->rc = CO_RC(MONITOR_NOT_LOADED);
			break;
		}

		if (!opened->monitor) {
			params->rc = CO_RC(MONITOR_NOT_LOADED);
			break;
		}

		in_size -= sizeof(*params);

		params->rc = co_monitor_ioctl(opened->monitor,
					      params,
					      in_size,
					      out_size,
					      return_size,
					      opened);
		break;

	}
	default:
		return CO_RC(ERROR);
	}

	return CO_RC(OK);
}
