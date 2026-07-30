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

co_rc_t co_manager_test_space(co_manager_handle_t handle,
			      co_manager_ioctl_test_space_t* out)
{
	unsigned long returned = 0;

	return co_os_manager_ioctl(handle, CO_MANAGER_IOCTL_TEST_SPACE,
				   out, sizeof(*out), out, sizeof(*out), &returned);
}

co_rc_t co_manager_test_switch(co_manager_handle_t handle,
			       co_manager_ioctl_test_switch_t* out,
			       int mode)
{
	unsigned long returned = 0;

	return co_os_manager_ioctl(handle,
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
