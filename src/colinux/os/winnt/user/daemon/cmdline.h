/*
 * This source code is a part of coLinux source package.
 *
 * Dan Aloni <da-x@colinux.org>, 2004 (c)
 *
 * The code is licensed under the GPL. See the COPYING file at
 * the root directory.
 *
 */

#ifndef __CO_OS_USER_WINNT_DAEMON_CMDLINE_H__
#define __CO_OS_USER_WINNT_DAEMON_CMDLINE_H__

#include <colinux/common/common.h>
#include <colinux/user/cmdline.h>

typedef struct co_winnt_parameters {
	bool_t install_service;
	bool_t remove_service;
	bool_t run_service;
	bool_t install_driver;
	bool_t status_driver;
	bool_t probe_va;
	bool_t probe_passage;
	bool_t probe_sweep;
	bool_t load_kernel;
	char   load_kernel_arg[0x100];
	bool_t call_kernel;
	char   call_kernel_arg[0x100];
	bool_t enter_kernel;
	char   enter_kernel_arg[0x100];
	bool_t dump_vmlinux;
	char   dump_vmlinux_arg[0x100];
	bool_t save_state;
	bool_t restore_state;
	bool_t test_switch;
	bool_t test_roundtrip;
	bool_t test_fault;
	bool_t test_resume;
	bool_t test_space;
	bool_t test_pagefault;
	bool_t test_badstack;
	bool_t test_guest;
	bool_t test_guestfault;
	char   probe_va_arg[64];
	bool_t show_status;
	bool_t remove_driver;
	char service_name[128];
} co_winnt_parameters_t;

extern void co_winnt_daemon_syntax(void);
extern co_rc_t co_winnt_daemon_parse_args(co_command_line_params_t cmdline, co_winnt_parameters_t *winnt_parameters);

#endif
