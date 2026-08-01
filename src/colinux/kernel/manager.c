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
#include "console.h"

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
	 * finishes before the tables go away.
	 */
	co_console_set_address(0);

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

	co_console_free();

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

static co_rc_t co_manager_close_(co_manager_t*          manager,
				 co_manager_open_desc_t opened)
{
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

	co_os_mutex_acquire(opened->lock);
	opened->ref_count--;
	close = (opened->ref_count == 0);
	co_os_mutex_release(opened->lock);

	if (close) {
		return co_manager_close_(manager, opened);
	}

	return CO_RC(OK);
}

co_rc_t co_manager_open_desc_deactive_and_close(co_manager_t*	       manager,
						co_manager_open_desc_t opened)
{
	co_rc_t rc;

	opened->active = PFALSE;
	if (opened->monitor != NULL) {
		co_monitor_t* mon = opened->monitor;
		int           index;

		co_os_mutex_acquire(mon->connected_modules_write_lock);
		for (index = 0; index < CO_MONITOR_MODULES_COUNT; index++) {
			if (mon->connected_modules[index] != opened)
				continue;

			mon->connected_modules[index] = NULL;
			co_manager_close(manager, opened);
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

		params->rc = co_kload_read(manager, params->va, params->data,
					   params->size);
		*return_size = sizeof(*params) + params->size;
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
		unsigned long offered;

		if (in_size < sizeof(*params) || out_size < sizeof(*params))
			return CO_RC(INVALID_PARAMETER);

		*return_size = sizeof(*params);

		offered = params->in_size;
		if (offered > sizeof(params->in))
			offered = sizeof(params->in);

		params->rc = co_console_pump(manager,
					     params->in, offered, &params->in_taken,
					     params->out, sizeof(params->out),
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
		in.step               = params->step;
		in.batch              = params->batch;
		in.kernel_table_count = params->kernel_table_count;
		co_memcpy(in.kernel_tables, params->kernel_tables,
			  sizeof(in.kernel_tables));
		in.ex_table_start = params->ex_table_start;
		in.ex_table_stop  = params->ex_table_stop;
		in.passage_symbol_va  = params->passage_symbol_va;
		co_console_set_address(params->console_io_va);
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
		 * Retire the console before the address space it reads through
		 * is freed. A console client is another process entirely and
		 * would otherwise keep walking page tables that have been
		 * returned to the host -- which is a use-after-free in a
		 * driver, so it bugchecks rather than fails.
		 */
		co_console_set_address(0);
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
