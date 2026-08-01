/*
 * This source code is a part of coLinux source package.
 *
 * Dan Aloni <da-x@colinux.org>, 2003 (c)
 *
 * Service support by Jaroslaw Kowalski <jaak@zd.com.pl>, 2004 (c)
 * Driver service separation by Daniel R. Slater <dan_slater@yahoo.com>, 2004 (c)
 *
 * The code is licensed under the GPL. See the COPYING file at
 * the root directory.
 *
 */

/* Main program of the "colinux-daemon.exe" */

#include <stdio.h>
#include <windows.h>
#include <stdarg.h>

#include <colinux/common/version.h>
#include <colinux/common/libc.h>
#include <colinux/user/daemon.h>
#include <colinux/user/debug.h>
#include <colinux/user/monitor.h>
#include <colinux/user/manager.h>
#include <colinux/user/cmdline.h>
#include <colinux/os/user/manager.h>
#include <colinux/os/user/misc.h>

#include "cmdline.h"
#include "console.h"
#include "misc.h"
#include "service.h"
#include "driver.h"
#include <colinux/user/elf_load.h>

COLINUX_DEFINE_MODULE("colinux-daemon");

bool_t co_running_as_service = PFALSE;

static co_daemon_t* g_daemon = NULL;
static bool_t	    stoped   = PFALSE;

/*
 * co_winnt_daemon_stop:
 *
 * This callback function is called when Windows sends a Stop request to the
 * coLinux service.
 * Or the user closed the command prompt of colinux-daemon.
 *
 **/
void co_winnt_daemon_stop(void)
{
	if (g_daemon != NULL) {
		co_daemon_send_shutdown(g_daemon);
		stoped = PTRUE;
	}
}

/*
 * co_winnt_daemon_ctrl_handler
 *
 * This callback function is called when the user closes the console window,
 * the system is shuting down or the user is loging off (and other control
 * events, like Ctrl+Break).
 * If we handle it, we must return TRUE, otherwise return false to let
 * windows or other control handler process the event.
 * This is needed because the default handler just calls ExitProcess(),
 * making sh*t out of the colinux system.
 */
static BOOL WINAPI co_winnt_daemon_ctrl_handler(DWORD dwCtrlType)
{
	switch (dwCtrlType)
	{
	case CTRL_C_EVENT:
	case CTRL_BREAK_EVENT:
		return TRUE;	// Don't let the user kill us that easily ;)
	case CTRL_LOGOFF_EVENT:
		// Only shutdown if we are not a service
		if (co_running_as_service)
		    return FALSE;
		// Shutdown to avoid corrupting the fs
	case CTRL_CLOSE_EVENT:
	case CTRL_SHUTDOWN_EVENT:
		// Shutdown CoLinux "gracefully"
		co_winnt_daemon_stop();
		return TRUE;
	}
	// Let windows or others handle it
	// In reality, there are no more events, so this should never
	// be executed (unless in future windows versions)
	return FALSE;
}

/*
 * A positive decimal count from the command line, or false.
 *
 * Two options want one and neither wants a partial answer: a mistyped digit
 * silently becoming a smaller number would change what a run means without
 * saying so.
 */
static bool_t co_parse_count(const char* s, unsigned long* out)
{
	unsigned long v = 0;

	while (*s >= '0' && *s <= '9')
		v = v * 10 + (unsigned long)(*s++ - '0');

	if (*s || !v)
		return PFALSE;

	*out = v;
	return PTRUE;
}

co_rc_t co_winnt_daemon_main(co_start_parameters_t* start_parameters)
{
	co_rc_t rc;

	if (!start_parameters->config_specified  ||  start_parameters->show_help) {
		co_daemon_syntax();
		co_winnt_daemon_syntax();
		return CO_RC(OK);
	}

	/* Workaround multiprocessor bug */
	co_winnt_affinity_workaround();

	// Don't get aborted ;)
	SetConsoleCtrlHandler(co_winnt_daemon_ctrl_handler, TRUE);

	rc = co_daemon_create(start_parameters, &g_daemon);
	if (!CO_OK(rc))
		goto out;

	rc = co_daemon_start_monitor(g_daemon);
	if (!CO_OK(rc))
		goto out_destroy;

	rc = co_daemon_run(g_daemon);

	co_daemon_end_monitor(g_daemon);

out_destroy:
	co_daemon_destroy(g_daemon);

out:
	if (!CO_OK(rc)) {
		char buf[0x100];

		switch (CO_RC_GET_CODE(rc)) {
		case CO_RC_VERSION_MISMATCHED:
			strcpy(buf, "error driver version, please reinstall driver!");
			break;
		case CO_RC_OUT_OF_PAGES:
			strcpy(buf, "not enough physical memory available (try with a lower setting)");
			break;
		case CO_RC_ERROR_ACCESSING_DRIVER:
			strcpy(buf, "can't access CoLinuxDriver, please check status driver!");
			break;
		default:
			co_rc_format_error(rc, buf, sizeof(buf));
		}

		co_terminal_print("daemon: exit code %08x\n", (int)rc);
		co_terminal_print("daemon: %s\n", buf);
	}

	SetConsoleCtrlHandler( co_winnt_daemon_ctrl_handler, FALSE );

	return rc;
}

static void co_winnt_help(void)
{
	co_terminal_print("\n");
	co_terminal_print("NOTE: Run without arguments to receive help about command line syntax.\n");
}

static co_rc_t co_winnt_main(int argc, char *args[])
{
	co_rc_t rc = CO_RC_OK;
	co_winnt_parameters_t winnt_parameters;
	co_start_parameters_t start_parameters;
	co_command_line_params_t cmdline;

	co_memset(&start_parameters, 0, sizeof(start_parameters));
	co_memset(&winnt_parameters, 0, sizeof(winnt_parameters));

	co_daemon_print_header();

	co_winnt_change_directory_for_service(argc, args);

	rc = co_cmdline_params_alloc(args, argc, &cmdline);
	if (!CO_OK(rc)) {
		co_terminal_print("daemon: error parsing arguments\n");
		return rc;
	}

	rc = co_winnt_daemon_parse_args(cmdline, &winnt_parameters);
	if (!CO_OK(rc)) {
		co_terminal_print("daemon: error parsing parameters\n");
		co_winnt_help();
		return CO_RC(ERROR);
	}

	rc = co_daemon_parse_args(cmdline, &start_parameters);
	if (!CO_OK(rc) || start_parameters.show_help){
		if (!CO_OK(rc)) {
			co_terminal_print("daemon: error parsing parameters\n");
		}
		co_winnt_help();
		return CO_RC(ERROR);
	}

	rc = co_cmdline_params_check_for_no_unparsed_parameters(cmdline, PFALSE);
	if (!CO_OK(rc)) {
		co_winnt_help();
		co_terminal_print("\n");
		co_cmdline_params_check_for_no_unparsed_parameters(cmdline, PTRUE);
		return CO_RC(ERROR);
	}

	if (winnt_parameters.status_driver) {
		return co_winnt_status_driver(1); // arg 1 = View all driver details
	}

	if (winnt_parameters.test_guestfault)
		return co_winnt_test_switch(7);

	if (winnt_parameters.test_guest)
		return co_winnt_test_switch(6);

	if (winnt_parameters.test_badstack)
		return co_winnt_test_switch(5);

	if (winnt_parameters.test_pagefault)
		return co_winnt_test_switch(4);

	if (winnt_parameters.test_space)
		return co_winnt_test_space();

	if (winnt_parameters.test_resume)
		return co_winnt_test_switch(3);

	if (winnt_parameters.test_switch || winnt_parameters.test_roundtrip ||
	    winnt_parameters.test_fault) {
		return co_winnt_test_switch(winnt_parameters.test_fault ? 2 :
					    winnt_parameters.test_roundtrip ? 1 : 0);
	}

	if (winnt_parameters.save_state || winnt_parameters.restore_state) {
		return co_winnt_save_state(winnt_parameters.restore_state);
	}

	if (winnt_parameters.probe_passage) {
		return co_winnt_probe_passage();
	}

	if (winnt_parameters.console) {
		unsigned long port = 0;

		if (!co_parse_count(winnt_parameters.console_arg, &port) ||
		    port == 0 || port > 65535) {
			co_terminal_print("--console wants a TCP port number\n");
			return CO_RC(INVALID_PARAMETER);
		}

		return co_winnt_console_server((unsigned short)port);
	}

	if (winnt_parameters.boot_kernel) {
		unsigned long limit = 0, batch = 0;

		if (winnt_parameters.max_switches &&
		    !co_parse_count(winnt_parameters.max_switches_arg, &limit)) {
			co_terminal_print("--max-switches wants a positive decimal count\n");
			return CO_RC(INVALID_PARAMETER);
		}

		if (winnt_parameters.batch &&
		    !co_parse_count(winnt_parameters.batch_arg, &batch)) {
			co_terminal_print("--batch wants a positive decimal count\n");
			return CO_RC(INVALID_PARAMETER);
		}

		return co_elf_load_into_guest(winnt_parameters.boot_kernel_arg, 3,
					      limit, batch,
					      winnt_parameters.cobd0 ?
						winnt_parameters.cobd0_arg : NULL,
					      winnt_parameters.init ?
						winnt_parameters.init_arg : NULL);
	}

	if (winnt_parameters.call_kernel) {
		return co_elf_load_into_guest(winnt_parameters.call_kernel_arg, 2, 0, 0, NULL, NULL);
	}

	if (winnt_parameters.enter_kernel) {
		return co_elf_load_into_guest(winnt_parameters.enter_kernel_arg, 1, 0, 0, NULL, NULL);
	}

	if (winnt_parameters.load_kernel) {
		return co_elf_load_into_guest(winnt_parameters.load_kernel_arg, 0, 0, 0, NULL, NULL);
	}

	if (winnt_parameters.net_dump) {
		return co_elf_net_dump_live();
	}

	if (winnt_parameters.net_take) {
		return co_elf_net_take_live(winnt_parameters.net_take_arg);
	}

	if (winnt_parameters.stop) {
		co_manager_handle_t handle;
		int was_running = 0;
		co_rc_t rc;

		handle = co_os_manager_open();
		if (!handle) {
			co_terminal_print("stop: cannot open the driver -- is it loaded?\n");
			return CO_RC(ERROR);
		}

		rc = co_manager_kstop(handle, &was_running);
		co_os_manager_close(handle);

		if (!CO_OK(rc)) {
			co_terminal_print("stop: ioctl failed (rc %x)\n", (int)rc);
			return rc;
		}

		if (was_running)
			co_terminal_print("stop: a boot loop was running; it has been"
					  " asked to end and will report as usual\n");
		else
			co_terminal_print("stop: no boot loop is running\n");
		return CO_RC(OK);
	}

	if (winnt_parameters.dump_vmlinux) {
		return co_elf_dump(winnt_parameters.dump_vmlinux_arg);
	}

	if (winnt_parameters.probe_sweep) {
		return co_winnt_probe_sweep();
	}

	if (winnt_parameters.probe_va) {
		return co_winnt_probe_va(winnt_parameters.probe_va_arg);
	}

	if (winnt_parameters.install_driver) {
		rc = co_winnt_install_driver();
		if (CO_OK(rc)) {
			co_terminal_print("daemon: driver installed\n");
		}
		return rc;
	}

	if (winnt_parameters.install_service) {
		char *szCmdLine, *p, **pp;
		int size;

		if (!start_parameters.config_specified) {
			co_terminal_print("daemon: config not specified\n");
			return CO_RC(ERROR);
		}

		// Create single command line back
		for (size = 0, pp = args; *pp; pp++)
			size += 1+strlen(*pp);
		szCmdLine = malloc(size);
		for (p = szCmdLine, pp = args; *pp; pp++)
			p += snprintf(p, size - (p - szCmdLine), (p == szCmdLine) ? "%s" : " %s", *pp);

		return co_winnt_daemon_install_as_service(winnt_parameters.service_name,
							  szCmdLine,
							  start_parameters.network_types);
	}

	if (winnt_parameters.remove_service) {
		return co_winnt_daemon_remove_service(winnt_parameters.service_name);
	}

	if (winnt_parameters.remove_driver) {
		return co_winnt_remove_driver();
	}

	if (winnt_parameters.run_service) {
		co_running_as_service = PTRUE;

		co_terminal_print("colinux: running as service '%s'\n",
				  winnt_parameters.service_name);
		if (start_parameters.launch_console)
		{
			co_terminal_print("colinux: not spawning a console, because we're running as service.\n");
			start_parameters.launch_console = PFALSE;
		}

		return co_winnt_daemon_initialize_service(&start_parameters);
	}

	if (!start_parameters.config_specified){
		if (!start_parameters.cmdline_config) {
			co_daemon_syntax();
			co_winnt_daemon_syntax();
		}
		return CO_RC(ERROR);
	}

	return co_winnt_daemon_main(&start_parameters);
}

int main(int argc, char *argv[])
{
	co_rc_t rc;
	int ret;

	co_debug_start();

	rc = co_winnt_main(argc-1, argv+1);

	// Translate retcode into errorlevel, for --status-driver
	ret = CO_RC_GET_CODE(rc);
	switch (ret) {
	case CO_RC_OK:				//  0: ok, no error
	case CO_RC_VERSION_MISMATCHED:		//  3: co_manager_status
	case CO_RC_ERROR_ACCESSING_DRIVER:	// 14: driver not installed
		break;
	default:
		ret = 1;
	}

	co_debug("rc=%08x exit=%d", (int)rc, -ret);
	co_debug_end();

	return -ret;
}
