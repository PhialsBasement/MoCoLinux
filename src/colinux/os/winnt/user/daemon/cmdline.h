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
#include <colinux/common/ioctl.h>
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
	bool_t boot_kernel;
	char   boot_kernel_arg[0x100];
	/*
	 * How many world switches --boot-kernel is allowed before it stops on
	 * purpose. The point is bisection: when the guest takes the machine down
	 * hard there is no bugcheck, no dump, and the driver's debug ring never
	 * gets drained -- the only channel that survives is the ioctl return, and
	 * that only comes back on a run that stops in time. Halving the count until
	 * the box lives, then walking it up, names the exact instruction.
	 */
	bool_t max_switches;
	char   max_switches_arg[0x20];
	/*
	 * Instructions the guest steps per crossing. The default is chosen to
	 * make stepping cheap; lowering it to 1 restores one world switch per
	 * instruction, which is the right thing when the question is "which
	 * instruction" rather than "how far".
	 */
	bool_t batch;
	char   batch_arg[0x20];
	/*
	 * Usable guest pseudo RAM in megabytes. The host backs it with scattered
	 * nonpaged-pool pages; a flag keeps capacity a runtime choice rather than
	 * requiring a cross-compile.
	 */
	bool_t mem;
	char   mem_arg[0x20];
	/*
	 * Turn the cooperative timer off, so the host never interrupts a running
	 * guest. Exists to answer one question without a rebuild: is a failure
	 * the injection, or something else? Without it the guest reverts to
	 * ticks at the idle boundary and the exit-to-user drain -- preemption of
	 * a userspace spinner goes away, everything else keeps working.
	 */
	bool_t no_copic;
	/*
	 * Force synchronous block I/O -- the old path that performs each cobd
	 * transfer inline on the monitor thread, freezing the single-CPU guest
	 * for its duration (defect 3). Async is the default; this is the A/B
	 * escape hatch and the known-good fallback.
	 */
	bool_t sync_cobd;
	/*
	 * Backing store for each cobd unit. Unit 0 is the guest's root device
	 * and is the one root= names; the rest are ordinary disks. An NT object
	 * path: an image file as \??\F:\xfer\root.img, or a raw partition as
	 * \??\\PhysicalDrive0\Partition3. The driver opens it, so it is the
	 * driver's idea of the path that matters, not the daemon's.
	 *
	 * Four of them, because the driver (CO_COBD_MAX_UNITS) and the guest
	 * (COBD_MAX_UNITS) have both carried four since cobd was written and
	 * only the daemon ever knew about one. A second disk is what lets a
	 * running guest build a root filesystem for the next one, which beats
	 * pushing a multi-gigabyte image to a box that cannot buffer it.
	 */
	/*
	 * Serve the guest's console on this TCP port and nothing else. A second
	 * process alongside the one running the guest; see console.c.
	 */
	/* Which init the guest runs; empty means /sbin/init. */
	bool_t init;
	char   init_arg[0x100];

	bool_t console;
	char   console_arg[0x20];
	/*
	 * Start one application inside the running guest and return. What the
	 * desktop shortcuts use, so a launcher needs no scripting host and no
	 * Python -- neither of which a stock XP has.
	 */
	bool_t run;
	char   run_arg[0x200];
	bool_t cobd[CO_COBD_MAX_UNITS];
	char   cobd_arg[CO_COBD_MAX_UNITS][0x200];
	bool_t call_kernel;
	char   call_kernel_arg[0x100];
	bool_t enter_kernel;
	char   enter_kernel_arg[0x100];
	bool_t dump_vmlinux;
	char   dump_vmlinux_arg[0x100];
	bool_t stop;
	bool_t net_dump;
	bool_t net_take;
	char   net_take_arg[0x20];
	bool_t net_peer;
	char   net_peer_arg[0x20];
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
