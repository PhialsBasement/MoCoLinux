/*
 * This source code is a part of coLinux source package.
 *
 * Dan Aloni <da-x@colinux.org>, 2003-2004 (c)
 *
 * The code is licensed under the GPL. See the COPYING file at
 * the root directory.
 */

#include <colinux/common/libc.h>
#include <colinux/common/version.h>
#include <colinux/os/kernel/alloc.h>
#include <colinux/os/kernel/monitor.h>
#include <colinux/os/kernel/manager.h>
#include <colinux/os/kernel/misc.h>
#include <colinux/os/kernel/mutex.h>
#include <colinux/arch/mmu.h>
#include <colinux/arch/probe.h>

#include "manager.h"
#include "monitor.h"
#include "pages.h"
#include "reversedpfns.h"

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

	co_debug("machine RAM use limit: %ld MB" , manager->hostmem_usage_limit);

	manager->hostmem_usage_limit <<= 20; /* Megify */
}

co_rc_t co_manager_load(co_manager_t *manager)
{
	co_rc_t rc;

	co_memset(manager, 0, sizeof(*manager));

	co_debug_startup();
	co_debug("loaded to host kernel");

	co_list_init(&manager->opens);
	co_list_init(&manager->monitors);

	rc = co_os_mutex_create(&manager->lock);
	if (!CO_OK(rc))
		return rc;

	rc = co_os_physical_memory_pages(&manager->hostmem_pages);
	if (!CO_OK(rc))
		goto out_err_mutex;

	/* Calculate amount in mega bytes, not overruns the 4GB limit of unsigned long integer */
	manager->hostmem_amount = manager->hostmem_pages >> (20-CO_ARCH_PAGE_SHIFT);
	co_debug("machine has %ld MB of RAM", manager->hostmem_amount);

	if (manager->hostmem_pages > 0x100000) {
		co_debug_error("error, machines with more than 4GB are not currently supported");
		rc = CO_RC(ERROR);
		goto out_err_mutex;
	}

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
			co_debug_error("monitor ioctl too small! (%ld < %d)",
			               in_size, sizeof(*params));
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
