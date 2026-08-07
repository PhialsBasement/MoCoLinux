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
/* End a running boot loop now; was_running says whether one existed. */
extern co_rc_t co_manager_kstop(co_manager_handle_t handle, int* was_running);

/* Deliver one frame to the guest via the RX ring. */
/* Batch of length-prefixed records; taken reports how many the ring accepted. */
extern co_rc_t co_manager_conet_put(co_manager_handle_t handle,
				    const unsigned char* data, unsigned int size,
				    unsigned int frames, unsigned int* taken);

/* Consume the TX ring: advance tx_tail (forward only, at most to head). */
extern co_rc_t co_manager_conet_take(co_manager_handle_t handle, unsigned int new_tail,
				     unsigned int* tx_head, unsigned int* tx_tail,
				     unsigned int* rx_head, unsigned int* rx_tail);

/* Read-only live window into the guest's TX network ring. */
extern co_rc_t co_manager_conet_dump(co_manager_handle_t handle,
				     unsigned int* tx_head, unsigned int* tx_tail,
				     unsigned int* rx_head, unsigned int* rx_tail,
				     unsigned int start, unsigned char* data,
				     unsigned int* size);

extern co_rc_t co_manager_kread(co_manager_handle_t handle, unsigned long long va,
				void* data, unsigned long size);
extern co_rc_t co_manager_kmap(co_manager_handle_t handle, unsigned long max_slice,
			       co_manager_ioctl_kmap_t* out);
extern co_rc_t co_manager_kunmap(co_manager_handle_t handle,
				 unsigned long* released_out);
extern co_rc_t co_manager_vgpu_address(co_manager_handle_t handle,
				       unsigned long long* va_out);
extern co_rc_t co_manager_vgpu_wake(co_manager_handle_t handle);
extern co_rc_t co_manager_kvirt_to_phys(co_manager_handle_t handle,
					unsigned long long va,
					unsigned long long* pa_out);
extern co_rc_t co_manager_kload_enter(co_manager_handle_t handle,
				      co_manager_ioctl_test_switch_t* out);
extern co_rc_t co_manager_kcall(co_manager_handle_t handle,
				co_manager_ioctl_kcall_t* out);
extern co_rc_t co_manager_kram(co_manager_handle_t handle,
			       co_manager_ioctl_kram_t* out);
/*
 * Attach a backing store to a cooperative block device unit. The driver opens
 * the path itself and holds it for the life of the guest; see ioctl.h.
 */
extern co_rc_t co_manager_console(co_manager_handle_t handle,
				  const char* in, unsigned long in_size,
				  unsigned long* in_taken,
				  char* out, unsigned long out_size,
				  unsigned long* out_len);
extern co_rc_t co_manager_cobd(co_manager_handle_t handle, int unit,
			       const char* path, unsigned long long* size_out);

extern co_rc_t co_manager_kboot(co_manager_handle_t handle,
				co_manager_ioctl_kboot_t* out);
extern co_rc_t co_manager_kload_end(co_manager_handle_t handle);

extern co_rc_t co_manager_test_space(co_manager_handle_t handle,
				     co_manager_ioctl_test_space_t* out);

extern co_rc_t co_manager_test_switch(co_manager_handle_t handle,
				      co_manager_ioctl_test_switch_t* out,
				      int mode);

extern co_rc_t co_manager_test_smp(co_manager_handle_t handle,
				   co_manager_ioctl_test_smp_t* out);

extern co_rc_t co_manager_kvcpu_run(co_manager_handle_t handle,
				    co_manager_ioctl_kvcpu_run_t* out);

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
