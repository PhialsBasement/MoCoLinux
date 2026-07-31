/*
 * This source code is a part of coLinux source package.
 *
 * Dan Aloni <da-x@colinux.org>, 2003 (c)
 *
 * The code is licensed under the GPL. See the COPYING file at
 * the root directory.
 *
 */

#ifndef __COLINUX_USER_MANAGER_H__
#define __COLINUX_USER_MANAGER_H__

#include <colinux/common/ioctl.h>
#include <colinux/os/user/manager.h>

extern co_rc_t co_manager_io_monitor(co_manager_handle_t handle,
				     co_monitor_ioctl_op_t op,
				     co_manager_ioctl_monitor_t *ioctl,
				     unsigned long in_size,
				     unsigned long out_size);

extern co_rc_t co_manager_io_monitor_unisize(co_manager_handle_t handle,
					     co_monitor_ioctl_op_t op,
					     co_manager_ioctl_monitor_t *ioctl,
					     unsigned long size);

extern co_rc_t co_manager_status(co_manager_handle_t handle,
				 co_manager_ioctl_status_t *status);

extern co_rc_t co_manager_info(co_manager_handle_t handle,
				 co_manager_ioctl_info_t *status);

extern co_rc_t co_manager_probe_va(co_manager_handle_t handle,
				   co_manager_ioctl_probe_va_t* probe);

extern co_rc_t co_manager_probe_passage(co_manager_handle_t handle,
					co_manager_ioctl_probe_passage_t* probe);

extern co_rc_t co_manager_save_state(co_manager_handle_t handle,
				     co_manager_ioctl_save_state_t* out);

extern co_rc_t co_manager_kload_begin(co_manager_handle_t handle,
				      unsigned long long min_va, unsigned long long max_va);
extern co_rc_t co_manager_kload_chunk(co_manager_handle_t handle, unsigned long long va,
				      const void* data, unsigned long size, int zero);
extern co_rc_t co_manager_kload_verify(co_manager_handle_t handle,
				       co_manager_ioctl_kload_verify_t* out);
extern co_rc_t co_manager_kload_enter(co_manager_handle_t handle,
				      co_manager_ioctl_test_switch_t* out);
extern co_rc_t co_manager_kcall(co_manager_handle_t handle,
				co_manager_ioctl_kcall_t* out);
extern co_rc_t co_manager_kram(co_manager_handle_t handle,
			       co_manager_ioctl_kram_t* out);
extern co_rc_t co_manager_kboot(co_manager_handle_t handle,
				co_manager_ioctl_kboot_t* out);
extern co_rc_t co_manager_kload_end(co_manager_handle_t handle);

extern co_rc_t co_manager_test_space(co_manager_handle_t handle,
				     co_manager_ioctl_test_space_t* out);

extern co_rc_t co_manager_test_switch(co_manager_handle_t handle,
				      co_manager_ioctl_test_switch_t* out,
				      int mode);

extern void co_manager_debug(co_manager_handle_t handle,
			     const char *buf, long size);

extern co_rc_t co_manager_debug_reader(co_manager_handle_t handle,
				       co_manager_ioctl_debug_reader_t *debug_reader);

#ifdef COLINUX_DEBUG
extern co_rc_t co_manager_debug_levels(co_manager_handle_t handle,
				       co_manager_ioctl_debug_levels_t *levels);
#endif

extern co_rc_t co_manager_attach(co_manager_handle_t handle, co_manager_ioctl_attach_t *params);

co_rc_t co_manager_monitor_list( co_manager_handle_t handle, co_manager_ioctl_monitor_list_t *list );

#endif
