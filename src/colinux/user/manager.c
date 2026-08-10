/*
 * This source code is a part of coLinux source package.
 *
 * Dan Aloni <da-x@colinux.org>, 2003 (c)
 *
 * The code is licensed under the GPL. See the COPYING file at
 * the root directory.
 *
 */

#include <colinux/os/user/misc.h>
#include <colinux/os/alloc.h>
#include <colinux/common/libc.h>

#include "manager.h"

co_rc_t co_manager_io_monitor(co_manager_handle_t	   handle,
			       co_monitor_ioctl_op_t	   op,
			       co_manager_ioctl_monitor_t* ioctl,
			       unsigned long 		   in_size,
			       unsigned long 		   out_size)
{
	unsigned long returned = 0;
	co_rc_t	      rc;

	ioctl->op = op;
	ioctl->rc = CO_RC_OK;

	rc = co_os_manager_ioctl(handle,
				 CO_MANAGER_IOCTL_MONITOR,
				 ioctl,
				 in_size,
				 ioctl,
				 out_size,
				 &returned);
	if (!CO_OK(rc))
		return rc;

	return ioctl->rc;
}

co_rc_t co_manager_io_monitor_unisize(co_manager_handle_t         handle,
				      co_monitor_ioctl_op_t       op,
				      co_manager_ioctl_monitor_t* ioctl,
				      unsigned long               size)
{
	return co_manager_io_monitor(handle, op, ioctl, size, size);
}

co_rc_t co_manager_status(co_manager_handle_t handle, co_manager_ioctl_status_t* status)
{
	co_rc_t		rc;
	unsigned long 	returned = 0;

	rc = co_os_manager_ioctl(handle,
				 CO_MANAGER_IOCTL_STATUS,
				 status,
				 sizeof(*status),
				 status,
				 sizeof(*status),
				 &returned);
	if (!CO_OK(rc))
		return rc;

	if (status->periphery_api_version != CO_LINUX_PERIPHERY_API_VERSION) {
		co_terminal_print("colinux: driver version mismatch: expected %d got %d\n",
				  CO_LINUX_PERIPHERY_API_VERSION, status->periphery_api_version);
		return CO_RC(VERSION_MISMATCHED);
	}

	return rc;
}

co_rc_t co_manager_probe_va(co_manager_handle_t handle,
			    co_manager_ioctl_probe_va_t* probe)
{
	co_rc_t rc;
	unsigned long returned = 0;

	rc = co_os_manager_ioctl(handle,
				 CO_MANAGER_IOCTL_PROBE_VA,
				 probe,
				 sizeof(*probe),
				 probe,
				 sizeof(*probe),
				 &returned);

	return rc;
}

co_rc_t co_manager_probe_passage(co_manager_handle_t handle,
				 co_manager_ioctl_probe_passage_t* probe)
{
	unsigned long returned = 0;

	return co_os_manager_ioctl(handle,
				   CO_MANAGER_IOCTL_PROBE_PASSAGE,
				   probe,
				   sizeof(*probe),
				   probe,
				   sizeof(*probe),
				   &returned);
}

co_rc_t co_manager_save_state(co_manager_handle_t handle,
			      co_manager_ioctl_save_state_t* out)
{
	unsigned long returned = 0;

	return co_os_manager_ioctl(handle, CO_MANAGER_IOCTL_SAVE_STATE,
				   out, sizeof(*out), out, sizeof(*out), &returned);
}

co_rc_t co_manager_kload_begin(co_manager_handle_t handle,
			       unsigned long long min_va, unsigned long long max_va,
			       unsigned long long ram_bytes)
{
	co_manager_ioctl_kload_begin_t params = {0, };
	unsigned long returned = 0;
	co_rc_t rc;

	params.min_va = min_va;
	params.max_va = max_va;
	params.ram_bytes = ram_bytes;

	rc = co_os_manager_ioctl(handle, CO_MANAGER_IOCTL_KLOAD_BEGIN,
				 &params, sizeof(params), &params, sizeof(params), &returned);
	return CO_OK(rc) ? params.rc : rc;
}

co_rc_t co_manager_kload_chunk(co_manager_handle_t handle, unsigned long long va,
			       const void* data, unsigned long size, int zero)
{
	co_manager_ioctl_kload_chunk_t* params;
	unsigned long returned = 0;
	unsigned long total = sizeof(*params) + (zero ? 0 : size);
	co_rc_t rc;

	params = co_os_malloc(total);
	if (!params)
		return CO_RC(OUT_OF_MEMORY);

	co_memset(params, 0, sizeof(*params));
	params->va   = va;
	params->size = size;
	params->zero = zero;
	if (!zero)
		co_memcpy(params->data, data, size);

	rc = co_os_manager_ioctl(handle, CO_MANAGER_IOCTL_KLOAD_CHUNK,
				 params, total, params, sizeof(*params), &returned);
	if (CO_OK(rc))
		rc = params->rc;

	co_os_free(params);
	return rc;
}

co_rc_t co_manager_conet_dump(co_manager_handle_t handle,
			      unsigned int* tx_head, unsigned int* tx_tail,
			      unsigned int* rx_head, unsigned int* rx_tail,
			      unsigned int start, unsigned char* data,
			      unsigned int* size)
{
	co_manager_ioctl_conet_dump_t* params;
	unsigned long returned = 0;
	unsigned long total = sizeof(*params) + *size;
	co_rc_t rc;

	params = co_os_malloc(total);
	if (!params)
		return CO_RC(OUT_OF_MEMORY);

	co_memset(params, 0, sizeof(*params));
	params->start = start;
	params->size  = *size;

	rc = co_os_manager_ioctl(handle, CO_MANAGER_IOCTL_CONET_DUMP,
				 params, sizeof(*params), params, total, &returned);
	if (CO_OK(rc))
		rc = params->rc;

	if (CO_OK(rc)) {
		*tx_head = params->tx_head;
		*tx_tail = params->tx_tail;
		*rx_head = params->rx_head;
		*rx_tail = params->rx_tail;
		*size    = params->size;
		if (params->size)
			co_memcpy(data, params->data, params->size);
	} else {
		*size = 0;
	}

	co_os_free(params);
	return rc;
}

/*
 * Hand a batch of length-prefixed records to the driver in one call.
 *
 * `data` is already in the ring's record format -- a 32-bit length, the frame,
 * padding to four bytes -- so the driver copies it in without reformatting and
 * there is one description of a record in the tree rather than two.
 *
 * `taken` reports how many were appended. Fewer than asked means the guest's
 * RX ring filled, which is back-pressure rather than an error: the caller keeps
 * the remainder and offers it again.
 */
co_rc_t co_manager_conet_put(co_manager_handle_t handle,
			     const unsigned char* data, unsigned int size,
			     unsigned int frames, unsigned int* taken)
{
	co_manager_ioctl_conet_put_t* params;
	unsigned long returned = 0;
	unsigned long total = sizeof(*params) + size;
	co_rc_t rc;

	*taken = 0;

	params = co_os_malloc(total);
	if (!params)
		return CO_RC(OUT_OF_MEMORY);

	co_memset(params, 0, sizeof(*params));
	params->size   = size;
	params->frames = frames;
	co_memcpy(params->data, data, size);

	rc = co_os_manager_ioctl(handle, CO_MANAGER_IOCTL_CONET_PUT,
				 params, total, params, sizeof(*params), &returned);
	if (CO_OK(rc))
		rc = params->rc;
	if (CO_OK(rc))
		*taken = params->taken;

	co_os_free(params);
	return rc;
}

co_rc_t co_manager_conet_take(co_manager_handle_t handle, unsigned int new_tail,
			      unsigned int* tx_head, unsigned int* tx_tail,
			      unsigned int* rx_head, unsigned int* rx_tail)
{
	co_manager_ioctl_conet_take_t params = {0, };
	unsigned long returned = 0;
	co_rc_t rc;

	params.new_tail = new_tail;

	rc = co_os_manager_ioctl(handle, CO_MANAGER_IOCTL_CONET_TAKE,
				 &params, sizeof(params), &params, sizeof(params),
				 &returned);
	if (CO_OK(rc))
		rc = params.rc;
	if (CO_OK(rc)) {
		*tx_head = params.tx_head;
		*tx_tail = params.tx_tail;
		*rx_head = params.rx_head;
		*rx_tail = params.rx_tail;
	}

	return rc;
}

co_rc_t co_manager_kstop(co_manager_handle_t handle, int* was_running)
{
	co_manager_ioctl_kstop_t params = {0, };
	unsigned long returned = 0;
	co_rc_t rc;

	rc = co_os_manager_ioctl(handle, CO_MANAGER_IOCTL_KSTOP,
				 &params, sizeof(params), &params, sizeof(params),
				 &returned);
	if (CO_OK(rc))
		rc = params.rc;
	if (was_running)
		*was_running = params.was_running;

	return rc;
}

co_rc_t co_manager_kread(co_manager_handle_t handle, unsigned long long va,
			 void* data, unsigned long size)
{
	co_manager_ioctl_kread_t* params;
	unsigned long returned = 0;
	unsigned long total = sizeof(*params) + size;
	co_rc_t rc;

	params = co_os_malloc(total);
	if (!params)
		return CO_RC(OUT_OF_MEMORY);

	co_memset(params, 0, sizeof(*params));
	params->va   = va;
	params->size = size;

	rc = co_os_manager_ioctl(handle, CO_MANAGER_IOCTL_KREAD,
				 params, sizeof(*params), params, total, &returned);
	if (CO_OK(rc))
		rc = params->rc;
	if (CO_OK(rc))
		co_memcpy(data, params->data, size);

	co_os_free(params);
	return rc;
}

/*
 * Map the guest's RAM into this process, in slices, and hand back where each
 * one landed. The mapping lives until KUNMAP or until this handle closes --
 * including the close the OS performs when the process dies, which is the
 * point: a daemon that crashes must not leave the driver unable to free guest
 * memory.
 */
co_rc_t co_manager_kmap(co_manager_handle_t handle, unsigned long max_slice,
			co_manager_ioctl_kmap_t* out)
{
	unsigned long returned = 0;
	co_rc_t rc;

	co_memset(out, 0, sizeof(*out));
	out->max_slice = max_slice;

	rc = co_os_manager_ioctl(handle, CO_MANAGER_IOCTL_KMAP,
				 out, sizeof(*out), out, sizeof(*out), &returned);
	if (CO_OK(rc))
		rc = out->rc;

	return rc;
}

/* Map only the fixed-size slice containing one guest-physical address. */
co_rc_t co_manager_kmap_range(co_manager_handle_t handle,
			      unsigned long long pa,
			      co_kmap_range_t* range_out,
			      int* reused_out)
{
	co_manager_ioctl_kmap_range_t params = {0, };
	unsigned long returned = 0;
	co_rc_t rc;

	params.pa = pa;
	rc = co_os_manager_ioctl(handle, CO_MANAGER_IOCTL_KMAP_RANGE,
				 &params, sizeof(params), &params, sizeof(params),
				 &returned);
	if (CO_OK(rc))
		rc = params.rc;
	if (CO_OK(rc) && range_out)
		*range_out = params.range;
	if (CO_OK(rc) && reused_out)
		*reused_out = params.reused ? 1 : 0;
	return rc;
}

co_rc_t co_manager_kunmap(co_manager_handle_t handle, unsigned long* released_out)
{
	co_manager_ioctl_kunmap_t params = {0, };
	unsigned long returned = 0;
	co_rc_t rc;

	rc = co_os_manager_ioctl(handle, CO_MANAGER_IOCTL_KUNMAP,
				 &params, sizeof(params), &params, sizeof(params),
				 &returned);
	if (CO_OK(rc))
		rc = params.rc;
	if (CO_OK(rc) && released_out)
		*released_out = params.released;

	return rc;
}

co_rc_t co_manager_vgpu_address(co_manager_handle_t handle, unsigned long long* va_out)
{
	co_manager_ioctl_vgpu_t params = {0, };
	unsigned long returned = 0;
	co_rc_t rc;

	rc = co_os_manager_ioctl(handle, CO_MANAGER_IOCTL_VGPU,
				 &params, sizeof(params), &params, sizeof(params),
				 &returned);
	if (CO_OK(rc))
		rc = params.rc;
	if (CO_OK(rc))
		*va_out = params.va;

	return rc;
}

/*
 * Ring the completion doorbell: the monitor's idle sleep wakes and re-enters
 * the guest, whose idle-boundary drain reaps what was just published. Called
 * by the GPU daemon once per service pass that produced completions, so the
 * cost -- one DeviceIoControl -- is per batch, not per request.
 */
co_rc_t co_manager_vgpu_wake(co_manager_handle_t handle)
{
	co_manager_ioctl_vgpu_wake_t params = {0, };
	unsigned long returned = 0;
	co_rc_t rc;

	rc = co_os_manager_ioctl(handle, CO_MANAGER_IOCTL_VGPU_WAKE,
				 &params, sizeof(params), &params, sizeof(params),
				 &returned);
	if (CO_OK(rc))
		rc = params.rc;

	return rc;
}

co_rc_t co_manager_kvirt_to_phys(co_manager_handle_t handle,
				 unsigned long long va, unsigned long long* pa_out)
{
	co_manager_ioctl_vgpu_t params = {0, };
	unsigned long returned = 0;
	co_rc_t rc;

	params.query_va = va;

	rc = co_os_manager_ioctl(handle, CO_MANAGER_IOCTL_VGPU,
				 &params, sizeof(params), &params, sizeof(params),
				 &returned);
	if (CO_OK(rc))
		rc = params.rc;
	if (CO_OK(rc))
		*pa_out = params.query_pa;

	return rc;
}

co_rc_t co_manager_kwindow(co_manager_handle_t handle,
			   const void* va, unsigned long long bytes,
			   unsigned long long* pseudo_pa_out)
{
	co_manager_ioctl_kwindow_t params = {0, };
	unsigned long returned = 0;
	co_rc_t rc;

	params.va = (unsigned long long)va;
	params.bytes = bytes;

	rc = co_os_manager_ioctl(handle, CO_MANAGER_IOCTL_KWINDOW,
				 &params, sizeof(params), &params, sizeof(params),
				 &returned);
	if (CO_OK(rc))
		rc = params.rc;
	if (CO_OK(rc))
		*pseudo_pa_out = params.pseudo_pa;

	return rc;
}

co_rc_t co_manager_kwindow_at(co_manager_handle_t handle,
			      const void* va, unsigned long long bytes,
			      unsigned long long pseudo_pa)
{
	co_manager_ioctl_kwindow_at_t params = {0, };
	unsigned long returned = 0;
	co_rc_t rc;

	params.va = (unsigned long long)va;
	params.bytes = bytes;
	params.pseudo_pa = pseudo_pa;

	rc = co_os_manager_ioctl(handle, CO_MANAGER_IOCTL_KWINDOW_AT,
				 &params, sizeof(params), &params, sizeof(params),
				 &returned);
	if (CO_OK(rc))
		rc = params.rc;

	return rc;
}

co_rc_t co_manager_window_bounds(co_manager_handle_t handle,
				 unsigned long long* base,
				 unsigned long long* top)
{
	co_manager_ioctl_vgpu_t params = {0, };
	unsigned long returned = 0;
	co_rc_t rc;

	rc = co_os_manager_ioctl(handle, CO_MANAGER_IOCTL_VGPU,
				 &params, sizeof(params), &params, sizeof(params),
				 &returned);
	if (CO_OK(rc))
		rc = params.rc;
	if (CO_OK(rc)) {
		*base = params.window_base;
		*top  = params.window_top;
	}

	return rc;
}

co_rc_t co_manager_timer_deadline_host(co_manager_handle_t handle,
				       unsigned long long* host_out)
{
	co_manager_ioctl_vgpu_t params = {0, };
	unsigned long returned = 0;
	co_rc_t rc;

	rc = co_os_manager_ioctl(handle, CO_MANAGER_IOCTL_VGPU,
				 &params, sizeof(params), &params, sizeof(params),
				 &returned);
	if (CO_OK(rc))
		rc = params.rc;
	if (CO_OK(rc))
		*host_out = params.timer_deadline_host;

	return rc;
}

co_rc_t co_manager_kunwindow(co_manager_handle_t handle,
			     unsigned long long pseudo_pa,
			     unsigned long long bytes)
{
	co_manager_ioctl_kunwindow_t params = {0, };
	unsigned long returned = 0;
	co_rc_t rc;

	params.pseudo_pa = pseudo_pa;
	params.bytes = bytes;

	rc = co_os_manager_ioctl(handle, CO_MANAGER_IOCTL_KUNWINDOW,
				 &params, sizeof(params), &params, sizeof(params),
				 &returned);
	if (CO_OK(rc))
		rc = params.rc;

	return rc;
}

co_rc_t co_manager_cobd(co_manager_handle_t handle, int unit, const char* path,
			unsigned long long* size_out)
{
	co_manager_ioctl_cobd_t params = {0, };
	unsigned long returned = 0;
	co_rc_t rc;

	params.unit = unit;
	co_snprintf(params.path, sizeof(params.path), "%s", path);

	rc = co_os_manager_ioctl(handle, CO_MANAGER_IOCTL_COBD,
				 &params, sizeof(params), &params, sizeof(params),
				 &returned);
	if (CO_OK(rc))
		rc = params.rc;
	if (CO_OK(rc))
		*size_out = params.size;

	return rc;
}

/*
 * One turn of the terminal: offer keystrokes, collect output.
 *
 * Safe to call while a boot is running in another thread or another process --
 * the driver takes no lock for this and the two rings have a single writer per
 * direction. That is the whole point: the monitor loop owns its ioctl for as
 * long as the guest runs.
 */
co_rc_t co_manager_console(co_manager_handle_t handle,
			   const char* in, unsigned long in_size,
			   unsigned long* in_taken,
			   char* out, unsigned long out_size,
			   unsigned long* out_len)
{
	co_manager_ioctl_console_t params = {0, };
	unsigned long returned = 0;
	co_rc_t rc;

	if (in_size > sizeof(params.in))
		in_size = sizeof(params.in);

	params.in_size  = in_size;
	params.out_size = out_size;
	if (in_size)
		co_memcpy(params.in, in, in_size);

	rc = co_os_manager_ioctl(handle, CO_MANAGER_IOCTL_CONSOLE,
				 &params, sizeof(params), &params, sizeof(params),
				 &returned);
	if (CO_OK(rc))
		rc = params.rc;
	if (!CO_OK(rc))
		return rc;

	*in_taken = params.in_taken;

	*out_len = params.out_len;
	if (*out_len > out_size)
		*out_len = out_size;
	if (*out_len)
		co_memcpy(out, params.out, *out_len);

	return rc;
}

co_rc_t co_manager_kload_verify(co_manager_handle_t handle,
				co_manager_ioctl_kload_verify_t* out)
{
	unsigned long returned = 0;

	return co_os_manager_ioctl(handle, CO_MANAGER_IOCTL_KLOAD_VERIFY,
				   out, sizeof(*out), out, sizeof(*out), &returned);
}

co_rc_t co_manager_kload_enter(co_manager_handle_t handle,
			       co_manager_ioctl_test_switch_t* out)
{
	unsigned long returned = 0;

	return co_os_manager_ioctl(handle, CO_MANAGER_IOCTL_KLOAD_ENTER,
				   out, sizeof(*out), out, sizeof(*out), &returned);
}

co_rc_t co_manager_kcall(co_manager_handle_t handle, co_manager_ioctl_kcall_t* out)
{
	unsigned long returned = 0;

	return co_os_manager_ioctl(handle, CO_MANAGER_IOCTL_KCALL,
				   out, sizeof(*out), out, sizeof(*out), &returned);
}

co_rc_t co_manager_kram(co_manager_handle_t handle, co_manager_ioctl_kram_t* out)
{
	unsigned long returned = 0;

	return co_os_manager_ioctl(handle, CO_MANAGER_IOCTL_KRAM,
				   out, sizeof(*out), out, sizeof(*out), &returned);
}

co_rc_t co_manager_kboot(co_manager_handle_t handle, co_manager_ioctl_kboot_t* out)
{
	unsigned long returned = 0;

	return co_os_manager_ioctl(handle, CO_MANAGER_IOCTL_KBOOT,
				   out, sizeof(*out), out, sizeof(*out), &returned);
}

co_rc_t co_manager_kload_end(co_manager_handle_t handle)
{
	unsigned long returned = 0;

	return co_os_manager_ioctl(handle, CO_MANAGER_IOCTL_KLOAD_END,
				   NULL, 0, NULL, 0, &returned);
}

co_rc_t co_manager_test_space(co_manager_handle_t handle,
			      co_manager_ioctl_test_space_t* out)
{
	unsigned long returned = 0;

	return co_os_manager_ioctl(handle, CO_MANAGER_IOCTL_TEST_SPACE,
				   out, sizeof(*out), out, sizeof(*out), &returned);
}

co_rc_t co_manager_kvcpu_run(co_manager_handle_t handle,
			     co_manager_ioctl_kvcpu_run_t* out)
{
	unsigned long returned = 0;
	co_rc_t rc;

	rc = co_os_manager_ioctl(handle, CO_MANAGER_IOCTL_KVCPU_RUN,
				 out, sizeof(*out), out, sizeof(*out), &returned);
	if (CO_OK(rc) && returned != sizeof(*out))
		return CO_RC(ERROR);
	return rc;
}

co_rc_t co_manager_test_smp(co_manager_handle_t handle,
			    co_manager_ioctl_test_smp_t* out)
{
	unsigned long returned = 0;
	co_rc_t rc;

	rc = co_os_manager_ioctl(handle, CO_MANAGER_IOCTL_TEST_SMP,
				 out, sizeof(*out), out, sizeof(*out), &returned);
	if (CO_OK(rc) && returned != sizeof(*out))
		return CO_RC(ERROR);
	return rc;
}

co_rc_t co_manager_test_switch(co_manager_handle_t handle,
			       co_manager_ioctl_test_switch_t* out,
			       int mode)
{
	unsigned long returned = 0;

	return co_os_manager_ioctl(handle,
				   mode == 7 ? CO_MANAGER_IOCTL_TEST_GUESTFAULT :
				   mode == 6 ? CO_MANAGER_IOCTL_TEST_GUEST :
				   mode == 5 ? CO_MANAGER_IOCTL_TEST_BADSTACK :
				   mode == 4 ? CO_MANAGER_IOCTL_TEST_PAGEFAULT :
				   mode == 3 ? CO_MANAGER_IOCTL_TEST_RESUME :
				   mode == 2 ? CO_MANAGER_IOCTL_TEST_FAULT :
				   mode == 1 ? CO_MANAGER_IOCTL_TEST_ROUNDTRIP
					     : CO_MANAGER_IOCTL_TEST_SWITCH,
				   out, sizeof(*out), out, sizeof(*out), &returned);
}

co_rc_t co_manager_info(co_manager_handle_t handle, co_manager_ioctl_info_t* info)
{
	co_rc_t rc;
	unsigned long returned = 0;

	rc = co_os_manager_ioctl(handle,
				 CO_MANAGER_IOCTL_INFO,
				 info,
				 sizeof(*info),
				 info,
				 sizeof(*info),
				 &returned);

	return rc;
}

void co_manager_debug(co_manager_handle_t handle, const char* buf, long size)
{
	unsigned long returned	= 0;
	unsigned long ret	= 0;

	co_os_manager_ioctl(handle,
			    CO_MANAGER_IOCTL_DEBUG,
			    (void*)buf,
			    size,
			    &ret,
			    sizeof(ret),
			    &returned);
}

co_rc_t co_manager_attach(co_manager_handle_t handle, co_manager_ioctl_attach_t *params)
{
	co_rc_t rc;
	unsigned long returned = 0;

	rc = co_os_manager_ioctl(handle, CO_MANAGER_IOCTL_ATTACH,
				 params, sizeof(*params), params, sizeof(*params), &returned);
	if (!CO_OK(rc))
		return rc;

	return params->rc;
}


co_rc_t co_manager_debug_reader(co_manager_handle_t handle, co_manager_ioctl_debug_reader_t *debug_reader)
{
	co_rc_t rc;
	unsigned long returned = 0;

	rc = co_os_manager_ioctl(handle, CO_MANAGER_IOCTL_DEBUG_READER,
				 debug_reader, sizeof(*debug_reader), debug_reader, sizeof(*debug_reader), &returned);

	if (!CO_OK(rc))
		return rc;

	return debug_reader->rc;
}


#ifdef COLINUX_DEBUG
co_rc_t co_manager_debug_levels(co_manager_handle_t handle, co_manager_ioctl_debug_levels_t *levels)
{
	co_rc_t rc;
	unsigned long returned = 0;

	rc = co_os_manager_ioctl(handle, CO_MANAGER_IOCTL_DEBUG_LEVELS,
				 levels, sizeof(*levels), levels, sizeof(*levels), &returned);

	return rc;
}
#endif

/**
 * Ask manager for the list of monitors running.
 *
 * The driver will fill the array with the PIDs of the registered
 * monitors.
 */
co_rc_t co_manager_monitor_list(co_manager_handle_t handle, co_manager_ioctl_monitor_list_t *list)
{
	co_rc_t rc;
	unsigned long returned = 0;

	rc = co_os_manager_ioctl(handle, CO_MANAGER_IOCTL_MONITOR_LIST,
 				 list, sizeof(*list), list, sizeof(*list), &returned);

	return rc;
}
