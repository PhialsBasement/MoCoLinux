/*
 * This source code is a part of coLinux source package.
 *
 * Dan Aloni <da-x@colinux.org>, 2003 (c)
 *
 * The code is licensed under the GPL. See the COPYING file at
 * the root directory.
 *
 */

#ifndef __COLINUX_USER_DAEMON_H__
#define __COLINUX_USER_DAEMON_H__

#include <colinux/common/config.h>
#include <colinux/common/messages.h>
#include <colinux/common/console.h>

#include "elf_load.h"
#include "monitor.h"
#include "cmdline.h"

/*
 * Console launched when "-t" is not given. Follows what the build produced:
 * colinux-console-fltk.exe is only built when FLTK has been supplied, and the
 * NT console has no external dependency. Override with -t, or suppress the
 * console entirely with -d.
 */
#ifdef CO_DEFAULT_CONSOLE_FLTK
#define CO_DEFAULT_CONSOLE "fltk"
#else
#define CO_DEFAULT_CONSOLE "nt"
#endif

typedef struct co_daemon_start_parameters {
	co_pathname_t config_path;
	bool_t launch_console;
	bool_t config_specified;
	bool_t show_help;
	char console[0x20];

	/* optionally gathered from command line */
	bool_t suppress_printk;
	bool_t cmdline_config;
	co_config_t config;
	bool_t pidfile_specified;
	co_pathname_t pidfile;
	int network_types;
} co_start_parameters_t;

typedef struct co_daemon {
	co_id_t id;
	co_start_parameters_t *start_parameters;
	co_config_t config;
	co_elf_data_t *elf_data;
	co_user_monitor_t *monitor;
	co_user_monitor_t *message_monitor;
	bool_t running;
	bool_t idle;
	char *buf;
	bool_t send_ctrl_alt_del;
	co_monitor_user_kernel_shared_t *shared;
	bool_t next_reboot_will_shutdown;
} co_daemon_t;

typedef enum {
	CO_CONNECTED_MODULE_STATE_NEW,
	CO_CONNECTED_MODULE_STATE_IDENTIFIED,
} co_connected_module_state_t;

#define CO_CONNECTED_MODULE_NAME_SIZE 0x30

extern void co_daemon_syntax(void);
extern void co_daemon_print_header(void);
extern co_rc_t co_daemon_create(co_start_parameters_t *start_parameters, co_daemon_t **co_daemon_out);
extern co_rc_t co_daemon_start_monitor(co_daemon_t *daemon);
extern co_rc_t co_daemon_run(co_daemon_t *daemon);
extern void co_daemon_end_monitor(co_daemon_t *daemon);
extern void co_daemon_destroy(co_daemon_t *daemon);
extern void co_daemon_send_shutdown(co_daemon_t *daemon);
extern co_rc_t co_daemon_parse_args(co_command_line_params_t cmdline, co_start_parameters_t *start_parameters);

#endif
