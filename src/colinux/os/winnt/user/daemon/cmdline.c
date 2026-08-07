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
	co_terminal_print("      --test-fault                 Raise #UD in the guest and catch it\n");
	co_terminal_print("      --test-guest                 Run a guest whose code and stack live\n");
	co_terminal_print("                                   outside the passage page\n");
	co_terminal_print("      --test-guestfault            The same, but faulting there\n");
	co_terminal_print("      --test-badstack              Fault with a destroyed stack pointer;\n");
	co_terminal_print("                                   only IST can deliver it\n");
	co_terminal_print("      --test-pagefault             Touch an unmapped guest address and\n");
	co_terminal_print("                                   report vector, error code and CR2\n");
	co_terminal_print("      --test-resume                Enter the guest repeatedly and require\n");
	co_terminal_print("                                   it to continue where it stopped\n");
	co_terminal_print("      --test-smp N                 Two threads, two processors, two guests:\n");
	co_terminal_print("                                   concurrent world switches with per-lane\n");
	co_terminal_print("                                   MSR sentinels. N crossings per lane\n");
	co_terminal_print("                                   (default 2000000). SMP groundwork.\n");
	co_terminal_print("      --test-space                 Build a guest address space and verify\n");
	co_terminal_print("                                   it by walking it. Never enters it.\n");
	co_terminal_print("      --test-roundtrip             Enter the guest address space and return\n");
	co_terminal_print("      --test-switch                Change CR3 into a one-page address space\n");
	co_terminal_print("                                   and back. Porting aid.\n");
	co_terminal_print("      --restore-state              Capture host CPU state and put it back\n");
	co_terminal_print("                                   in place. Writes CRs, GDT/IDT/TR, MSRs.\n");
	co_terminal_print("      --save-state                 Dump host CPU state as the switch would\n");
	co_terminal_print("                                   capture it. Porting aid, reads only.\n");
	co_terminal_print("      --probe-passage              Report where a passage page lands and\n");
	co_terminal_print("                                   whether it is executable. Porting aid.\n");
	co_terminal_print("      --load-kernel FILE           Load a vmlinux into a guest address\n");
	co_terminal_print("                                   space and verify it. Does not enter.\n");
	co_terminal_print("      --boot-kernel FILE           Enter co_arch_start_kernel and report\n");
	co_terminal_print("                                   what the kernel printed before stopping\n");
	co_terminal_print("      --batch N                    Instructions the guest steps per crossing\n");
	co_terminal_print("      --run CMD                    Start CMD in the running guest and return,\n");
	co_terminal_print("                                   with DISPLAY set for the host's X server.\n");
	co_terminal_print("                                   What the desktop shortcuts use.\n");
	co_terminal_print("      --sync-cobd                  Perform block I/O inline (freezes the guest\n");
	co_terminal_print("                                   for each transfer); async is the default.\n");
	co_terminal_print("      --no-copic                   Do not interrupt a running guest. Ticks then\n");
	co_terminal_print("                                   arrive only at idle and at exits to user mode,\n");
	co_terminal_print("                                   so a userspace spin loop cannot be preempted.\n");
	co_terminal_print("      --cpus N                     Guest processors (default 1). Each one is a\n");
	co_terminal_print("                                   host thread pinned to a host core for the\n");
	co_terminal_print("                                   whole run, so this is capped at cores-2:\n");
	co_terminal_print("                                   Windows and the GPU daemon need one each.\n");
	co_terminal_print("      --mem MB                     Guest RAM in megabytes (default 1024). Taken\n");
	co_terminal_print("                                   as unbroken 32 MB physical runs, so a large\n");
	co_terminal_print("                                   value on a fragmented host stalls it; short\n");
	co_terminal_print("                                   is reported, not fatal.\n");
	co_terminal_print("      --cobd0 PATH                 Backing store for the guest's root device\n");
	co_terminal_print("      --cobd1..3 PATH              Further disks, /dev/cobd1 and up. A blank\n");
	co_terminal_print("                                   image here is how a running guest builds a\n");
	co_terminal_print("                                   root filesystem for the next one.\n");
	co_terminal_print("      --init PATH                  What the guest runs as pid 1\n");
	co_terminal_print("                                   (default /sbin/init). /bin/sh answers\n");
	co_terminal_print("                                   \"does this root work\" on its own.\n");
	co_terminal_print("      --console PORT               Serve the running guest's terminal on a\n");
	co_terminal_print("                                   TCP port. A second process: the one that\n");
	co_terminal_print("                                   booted the guest is inside its ioctl.\n");
	co_terminal_print("                                   (default 4096). 1 means one world switch\n");
	co_terminal_print("                                   per instruction, which is 3.6us each.\n");
	co_terminal_print("      --max-switches N|none        Stop --boot-kernel after N world switches,\n");
	co_terminal_print("                                   or none for no limit (stop.bat ends it)\n");
	co_terminal_print("                                   (default 200000). Bisects a hard reset:\n");
	co_terminal_print("                                   a run that stops in time still reports.\n");
	co_terminal_print("      --call-kernel FILE           Load a vmlinux, then call memset and\n");
	co_terminal_print("                                   strlen out of it. Runs kernel code.\n");
	co_terminal_print("      --enter-kernel FILE          The same, then transfer control into\n");
	co_terminal_print("                                   the loaded image and back.\n");
	co_terminal_print("      --net-dump                   Print the running guest's network rings,\n");
	co_terminal_print("                                   read-only -- the guest cannot tell.\n");
	co_terminal_print("      --net-peer [SECONDS]         Answer ARP and ICMP echo for 10.0.2.2 out of\n");
	co_terminal_print("                                   the RX ring -- a stand-in for slirp, so ping\n");
	co_terminal_print("                                   works in the guest with no NAT code running.\n");
	co_terminal_print("      --net-take [NEWTAIL]         Print the TX ring, then consume it (advance\n");
	co_terminal_print("                                   tx_tail past what was printed). An explicit\n");
	co_terminal_print("                                   NEWTAIL tests the driver's validation.\n");
	co_terminal_print("      --stop                       End a running boot loop now. The run's own\n");
	co_terminal_print("                                   daemon prints its report and cleans up as\n");
	co_terminal_print("                                   usual; this only asks the loop to return.\n");
	co_terminal_print("      --dump-vmlinux FILE          Parse an ELF kernel image and print its\n");
	co_terminal_print("                                   sections and key symbols. Porting aid.\n");
	co_terminal_print("      --probe-sweep                Map which of the 256 kernel PML4 slots\n");
	co_terminal_print("                                   the host uses. x86-64 porting aid.\n");
	co_terminal_print("      --probe-va [ADDR]            Report what the host has mapped at a\n");
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
		"--test-fault",
		&winnt_parameters->test_fault);

	if (!CO_OK(rc))
		return rc;

	rc = co_cmdline_params_argumentless_parameter(
		cmdline,
		"--test-resume",
		&winnt_parameters->test_resume);

	if (!CO_OK(rc))
		return rc;

	rc = co_cmdline_params_argumentless_parameter(
		cmdline,
		"--test-guest",
		&winnt_parameters->test_guest);

	if (!CO_OK(rc))
		return rc;

	rc = co_cmdline_params_argumentless_parameter(
		cmdline,
		"--test-guestfault",
		&winnt_parameters->test_guestfault);

	if (!CO_OK(rc))
		return rc;

	rc = co_cmdline_params_argumentless_parameter(
		cmdline,
		"--test-badstack",
		&winnt_parameters->test_badstack);

	if (!CO_OK(rc))
		return rc;

	rc = co_cmdline_params_argumentless_parameter(
		cmdline,
		"--test-pagefault",
		&winnt_parameters->test_pagefault);

	if (!CO_OK(rc))
		return rc;

	rc = co_cmdline_params_argumentless_parameter(
		cmdline,
		"--test-space",
		&winnt_parameters->test_space);

	if (!CO_OK(rc))
		return rc;

	rc = co_cmdline_params_one_optional_arugment_parameter(
		cmdline, "--test-smp",
		&winnt_parameters->test_smp,
		winnt_parameters->test_smp_arg,
		sizeof(winnt_parameters->test_smp_arg));

	if (!CO_OK(rc))
		return rc;

	rc = co_cmdline_params_one_optional_arugment_parameter(
		cmdline, "--cpus",
		&winnt_parameters->cpus,
		winnt_parameters->cpus_arg,
		sizeof(winnt_parameters->cpus_arg));

	if (!CO_OK(rc))
		return rc;

	rc = co_cmdline_params_argumentless_parameter(
		cmdline,
		"--probe-sweep",
		&winnt_parameters->probe_sweep);

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
		"--net-dump",
		&winnt_parameters->net_dump);

	if (!CO_OK(rc))
		return rc;

	rc = co_cmdline_params_one_optional_arugment_parameter(
		cmdline, "--net-peer",
		&winnt_parameters->net_peer,
		winnt_parameters->net_peer_arg,
		sizeof(winnt_parameters->net_peer_arg));

	if (!CO_OK(rc))
		return rc;

	rc = co_cmdline_params_one_optional_arugment_parameter(
		cmdline, "--net-take",
		&winnt_parameters->net_take,
		winnt_parameters->net_take_arg,
		sizeof(winnt_parameters->net_take_arg));

	if (!CO_OK(rc))
		return rc;

	rc = co_cmdline_params_argumentless_parameter(
		cmdline,
		"--stop",
		&winnt_parameters->stop);

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
		cmdline, "--load-kernel",
		&winnt_parameters->load_kernel,
		winnt_parameters->load_kernel_arg,
		sizeof(winnt_parameters->load_kernel_arg));

	if (!CO_OK(rc))
		return rc;

	rc = co_cmdline_params_one_optional_arugment_parameter(
		cmdline, "--boot-kernel",
		&winnt_parameters->boot_kernel,
		winnt_parameters->boot_kernel_arg,
		sizeof(winnt_parameters->boot_kernel_arg));

	if (!CO_OK(rc))
		return rc;

	rc = co_cmdline_params_one_optional_arugment_parameter(
		cmdline, "--max-switches",
		&winnt_parameters->max_switches,
		winnt_parameters->max_switches_arg,
		sizeof(winnt_parameters->max_switches_arg));

	if (!CO_OK(rc))
		return rc;

	rc = co_cmdline_params_one_optional_arugment_parameter(
		cmdline, "--batch",
		&winnt_parameters->batch,
		winnt_parameters->batch_arg,
		sizeof(winnt_parameters->batch_arg));

	if (!CO_OK(rc))
		return rc;

	rc = co_cmdline_params_one_optional_arugment_parameter(
		cmdline, "--init",
		&winnt_parameters->init,
		winnt_parameters->init_arg,
		sizeof(winnt_parameters->init_arg));

	if (!CO_OK(rc))
		return rc;

	rc = co_cmdline_params_one_optional_arugment_parameter(
		cmdline, "--console",
		&winnt_parameters->console,
		winnt_parameters->console_arg,
		sizeof(winnt_parameters->console_arg));

	if (!CO_OK(rc))
		return rc;

	rc = co_cmdline_params_one_optional_arugment_parameter(
		cmdline, "--run",
		&winnt_parameters->run,
		winnt_parameters->run_arg,
		sizeof(winnt_parameters->run_arg));

	if (!CO_OK(rc))
		return rc;

	/*
	 * --cobd0 .. --cobd3. The option name is built rather than written out
	 * four times, so adding a unit is a change to CO_COBD_MAX_UNITS and
	 * nothing else -- the driver and the guest already take their bound
	 * from the same constant.
	 */
	{
		int unit;

		for (unit = 0; unit < CO_COBD_MAX_UNITS; unit++) {
			char opt[16];

			co_snprintf(opt, sizeof(opt), "--cobd%d", unit);

			rc = co_cmdline_params_one_optional_arugment_parameter(
				cmdline, opt,
				&winnt_parameters->cobd[unit],
				winnt_parameters->cobd_arg[unit],
				sizeof(winnt_parameters->cobd_arg[unit]));

			if (!CO_OK(rc))
				return rc;
		}
	}

	rc = co_cmdline_params_argumentless_parameter(
		cmdline, "--no-copic", &winnt_parameters->no_copic);

	if (!CO_OK(rc))
		return rc;

	rc = co_cmdline_params_argumentless_parameter(
		cmdline, "--sync-cobd", &winnt_parameters->sync_cobd);

	if (!CO_OK(rc))
		return rc;

	rc = co_cmdline_params_one_optional_arugment_parameter(
		cmdline, "--mem",
		&winnt_parameters->mem,
		winnt_parameters->mem_arg,
		sizeof(winnt_parameters->mem_arg));

	if (!CO_OK(rc))
		return rc;

	rc = co_cmdline_params_one_optional_arugment_parameter(
		cmdline, "--call-kernel",
		&winnt_parameters->call_kernel,
		winnt_parameters->call_kernel_arg,
		sizeof(winnt_parameters->call_kernel_arg));

	if (!CO_OK(rc))
		return rc;

	rc = co_cmdline_params_one_optional_arugment_parameter(
		cmdline, "--enter-kernel",
		&winnt_parameters->enter_kernel,
		winnt_parameters->enter_kernel_arg,
		sizeof(winnt_parameters->enter_kernel_arg));

	if (!CO_OK(rc))
		return rc;

	rc = co_cmdline_params_one_optional_arugment_parameter(
		cmdline, "--dump-vmlinux",
		&winnt_parameters->dump_vmlinux,
		winnt_parameters->dump_vmlinux_arg,
		sizeof(winnt_parameters->dump_vmlinux_arg));

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
