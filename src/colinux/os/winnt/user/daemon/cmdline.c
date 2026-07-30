/*
 * This source code is a part of coLinux source package.
 *
 * Dan Aloni <da-x@colinux.org>, 2004 (c)
 *
 * The code is licensed under the GPL. See the COPYING file at
 * the root directory.
 *
 */

#include <windows.h>

#include <colinux/common/common.h>
#include <colinux/user/daemon.h>
#include <colinux/os/user/misc.h>

#include "cmdline.h"

void co_winnt_daemon_syntax(void)
{
	co_daemon_print_header();
	co_terminal_print("\n");
	co_terminal_print("The following options are specific to Windows NT/XP/2000:\n");
	co_terminal_print("\n");
	co_terminal_print("      --install-service [name]     Install colinux-daemon.exe as an NT service\n");
	co_terminal_print("                                   (default service name: Cooperative Linux)\n");
	co_terminal_print("      --remove-service [name]      Remove colinux service\n");
	co_terminal_print("                                   (default service name: Cooperative Linux)\n");
	co_terminal_print("      --install-driver             Install the colinux-driver (linux.sys)\n");
	co_terminal_print("      --remove-driver              Uninstall (remove) the colinux-driver (linux.sys)\n");
	co_terminal_print("      --status-driver              Show status about the installed/running\n");
	co_terminal_print("      --test-roundtrip             Enter the guest address space and return\n");
	co_terminal_print("      --test-switch                Change CR3 into a one-page address space\n");
	co_terminal_print("                                   and back. Porting aid.\n");
	co_terminal_print("      --restore-state              Capture host CPU state and put it back\n");
	co_terminal_print("                                   in place. Writes CRs, GDT/IDT/TR, MSRs.\n");
	co_terminal_print("      --save-state                 Dump host CPU state as the switch would\n");
	co_terminal_print("                                   capture it. Porting aid, reads only.\n");
	co_terminal_print("      --probe-passage              Report where a passage page lands and\n");
	co_terminal_print("                                   whether it is executable. Porting aid.\n");
	co_terminal_print("      --probe-va[=ADDR]            Report what the host has mapped at a\n");
	co_terminal_print("                                   virtual address. x86-64 porting aid;\n");
	co_terminal_print("                                   defaults to the guest window base.\n");
	co_terminal_print("                                   driver\n");
}

co_rc_t co_winnt_daemon_parse_args(co_command_line_params_t cmdline, co_winnt_parameters_t *winnt_parameters)
{
	co_rc_t rc;

	/* Default settings */
	co_snprintf(winnt_parameters->service_name, sizeof(winnt_parameters->service_name), "Cooperative Linux");

	winnt_parameters->install_service = PFALSE;
	winnt_parameters->run_service = PFALSE;
	winnt_parameters->remove_service = PFALSE;
	winnt_parameters->install_driver = PFALSE;
	winnt_parameters->remove_driver = PFALSE;
	winnt_parameters->status_driver = PFALSE;

	rc = co_cmdline_params_one_optional_arugment_parameter(
		cmdline, "--install-service",
		&winnt_parameters->install_service,
		winnt_parameters->service_name,
		sizeof(winnt_parameters->service_name));

	if (!CO_OK(rc))
		return rc;

	rc = co_cmdline_params_one_optional_arugment_parameter(
		cmdline, "--remove-service",
		&winnt_parameters->remove_service,
		winnt_parameters->service_name,
		sizeof(winnt_parameters->service_name));

	if (!CO_OK(rc))
		return rc;

	rc = co_cmdline_params_one_optional_arugment_parameter(
		cmdline, "--run-service",
		&winnt_parameters->run_service,
		winnt_parameters->service_name,  /* service name is obsolate at running time */
		sizeof(winnt_parameters->service_name));

	if (!CO_OK(rc))
		return rc;

	rc = co_cmdline_params_argumentless_parameter(
		cmdline,
		"--install-driver",
		&winnt_parameters->install_driver);

	if (!CO_OK(rc))
		return rc;

	rc = co_cmdline_params_argumentless_parameter(
		cmdline,
		"--remove-driver",
		&winnt_parameters->remove_driver);

	if (!CO_OK(rc))
		return rc;

	rc = co_cmdline_params_argumentless_parameter(
		cmdline,
		"--status-driver",
		&winnt_parameters->status_driver);

	if (!CO_OK(rc))
		return rc;

	rc = co_cmdline_params_argumentless_parameter(
		cmdline,
		"--test-roundtrip",
		&winnt_parameters->test_roundtrip);

	if (!CO_OK(rc))
		return rc;

	rc = co_cmdline_params_argumentless_parameter(
		cmdline,
		"--test-switch",
		&winnt_parameters->test_switch);

	if (!CO_OK(rc))
		return rc;

	rc = co_cmdline_params_argumentless_parameter(
		cmdline,
		"--restore-state",
		&winnt_parameters->restore_state);

	if (!CO_OK(rc))
		return rc;

	rc = co_cmdline_params_argumentless_parameter(
		cmdline,
		"--save-state",
		&winnt_parameters->save_state);

	if (!CO_OK(rc))
		return rc;

	rc = co_cmdline_params_argumentless_parameter(
		cmdline,
		"--probe-passage",
		&winnt_parameters->probe_passage);

	if (!CO_OK(rc))
		return rc;

	rc = co_cmdline_params_one_optional_arugment_parameter(
		cmdline, "--probe-va",
		&winnt_parameters->probe_va,
		winnt_parameters->probe_va_arg,
		sizeof(winnt_parameters->probe_va_arg));

	if (!CO_OK(rc))
		return rc;

	return CO_RC(OK);
}
