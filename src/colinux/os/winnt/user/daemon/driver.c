/*
 * This source code is a part of coLinux source package.
 *
 * Dan Aloni <da-x@colinux.org>, 2003 (c)
 *
 * Service support by Jaroslaw Kowalski <jaak@zd.com.pl>, 2004 (c)
 * Driver service separation by Daniel R. Slater <dan_slater@yahoo.com>, 2004 (c)
 * Driver installation fixes by George Boutwell <george.boutwell@gmail.com>, 2004 (c)
 *
 * The code is licensed under the GPL. See the COPYING file at
 * the root directory.
 *
 */

#include <stdio.h>
#include <windows.h>
#include <shlwapi.h>

#include <colinux/common/common.h>
#include <colinux/common/ioctl.h>
#include <colinux/user/daemon.h>
#include <colinux/user/manager.h>
#include <colinux/user/debug.h>
#include <colinux/os/user/misc.h>
#include <colinux/os/winnt/kernel/driver.h>
#include <colinux/os/winnt/user/misc.h>
#include <colinux/os/current/os.h>

#include "main.h"
#include "cmdline.h"
#include "misc.h"
#include "driver.h"
#include "service.h"

static co_rc_t co_winnt_driver_remove_lowlevel(void);
static co_rc_t co_winnt_driver_install_lowlevel(void);

static co_rc_t co_win32_manager_is_installed(bool_t *installed)
{
	co_rc_t rc;

	rc = co_os_manager_is_installed(installed);
	if (!CO_OK(rc)) {
		if (CO_RC_GET_CODE(rc) == CO_RC_ACCESS_DENIED)
			co_terminal_print("access denied, not enough privileges\n");
		else
			co_terminal_print("error, unable to determine if driver is installed (rc %x)\n", (int)rc);
	}

	return rc;
}

/*
 * This function makes sure the driver is installed and initializes it.
 */
co_rc_t co_winnt_install_driver(void)
{
	co_rc_t rc;
	bool_t installed = PFALSE;
	co_manager_handle_t handle;

	rc = co_win32_manager_is_installed(&installed);
	if (!CO_OK(rc)) {
		return rc;
	}

	if (installed) {
		co_terminal_print("driver already installed\n");
		return CO_RC(OK);
	}

	rc = co_winnt_driver_install_lowlevel();
	if (!CO_OK(rc)) {
		co_terminal_print("cannot install (rc %x)\n", (int)rc);
		return CO_RC(ERROR);
	}

	handle = co_os_manager_open();
	if (handle == NULL) {
		co_terminal_print("error opening kernel driver\n");
		return CO_RC(ERROR_ACCESSING_DRIVER);
	}

	return CO_RC(OK);
}

co_rc_t co_winnt_remove_driver(void)
{
	co_rc_t rc;
	bool_t installed = PFALSE;
	co_manager_handle_t handle;
	co_manager_ioctl_status_t status = {0, };

	rc = co_win32_manager_is_installed(&installed);
	if (!CO_OK(rc))
		return rc;

	if (!installed) {
		co_terminal_print("driver not installed\n");
		return CO_RC(ERROR_ACCESSING_DRIVER);
	}

	handle = co_os_manager_open();
	if (!handle) {
		co_terminal_print("couldn't get driver handle, removing anyway\n");
		co_winnt_driver_remove_lowlevel();
		return CO_RC(ERROR_ACCESSING_DRIVER);
	}
	rc = co_manager_status(handle, &status);
	if (!CO_OK(rc)) {
		if (CO_RC_GET_CODE(rc) == CO_RC_VERSION_MISMATCHED) {
			co_terminal_print("driver version is %d while expected version %d\n",
					 status.periphery_api_version, CO_LINUX_PERIPHERY_API_VERSION);
		} else {
			co_terminal_print("detected an old buggy driver version\n");
		}
		co_terminal_print("Remove driver anyway\n");
		rc = CO_RC(ERROR_ACCESSING_DRIVER);
	} else
	if (status.monitors_count != 0) {
		co_terminal_print("monitors are running, cannot remove driver\n");
		co_os_manager_close(handle);
		return CO_RC(ERROR);
	}

	co_os_manager_close(handle);
	co_winnt_driver_remove_lowlevel();
	return rc;
}

co_rc_t co_winnt_status_driver(int verbose)
{
	co_rc_t rc;
	bool_t installed = PFALSE;
	co_manager_handle_t handle;
	co_manager_ioctl_status_t status = {0, };

	if (verbose) {
		co_terminal_print("checking if the driver is installed\n");
		strcpy(status.compile_time, "<unknown>");
	}

	rc = co_win32_manager_is_installed(&installed);
	if (!CO_OK(rc))
		return rc;

	if (!installed) {
		co_terminal_print("driver not installed\n");
		return CO_RC(ERROR_ACCESSING_DRIVER);
	}

	handle = co_os_manager_open();
	if (!handle) {
		co_terminal_print("couldn't get driver handle\n");
		return CO_RC(ERROR_MONITOR_NOT_LOADED);
	}

	rc = co_manager_status(handle, &status);
	if (!CO_OK(rc)) {
		if (verbose)
			co_terminal_print("couldn't get driver status (rc %x)\n", (int)rc);
		co_os_manager_close(handle);
		return rc;
	}

	if (verbose) {
		co_terminal_print("current state: %ld%s\n", status.state,
				  (status.state >= CO_MANAGER_STATE_INITIALIZED) ?
				  " (fully initialized)" : "");
		co_terminal_print("current number of monitors: %d\n", status.monitors_count);
		co_terminal_print("current linux api version: %d\n", status.linux_api_version);
		co_terminal_print("current periphery api version: %d\n", status.periphery_api_version);
		co_terminal_print("Driver compiled on: %s\n", status.compile_time);
	}
	co_os_manager_close(handle);

	return rc;
}

/*
 * Ask the driver what the host has mapped at a virtual address.
 *
 * An x86-64 porting aid. The passage page has to be reachable from both address
 * spaces; if the guest's window is also unused by the host, it can sit at the
 * same virtual address in both, which makes other_map zero and means the switch
 * never relocates its own instruction pointer. See doc/porting-x86_64 4.1.
 */
/*
 * Walk every PML4 slot in the kernel half and report which ones the host has
 * anything mapped in at all.
 *
 * One probe per slot, at the slot's own base address. That is enough: a slot's
 * PML4 entry is absent or present for the whole 512 GB it covers, so a single
 * miss at the base proves the entire slot is unclaimed. Slots that are present
 * say nothing about how much of the 512 GB is actually used -- only that the
 * host owns the range, which is all we need in order to stay out of it.
 *
 * Purely user-space: it reuses the existing per-address ioctl in a loop rather
 * than adding a kernel-side sweep, so the driver is untouched and this cannot
 * fault the machine.
 */
co_rc_t co_winnt_probe_sweep(void)
{
	co_rc_t rc;
	bool_t installed = PFALSE;
	co_manager_handle_t handle;
	int slot;
	int run_start = -1;
	bool_t run_present = PFALSE;
	int free_slots = 0;
	unsigned long long first_cr3 = 0;

	rc = co_win32_manager_is_installed(&installed);
	if (!CO_OK(rc))
		return rc;

	if (!installed) {
		co_terminal_print("driver not installed\n");
		return CO_RC(ERROR_ACCESSING_DRIVER);
	}

	handle = co_os_manager_open();
	if (!handle) {
		co_terminal_print("couldn't get driver handle\n");
		return CO_RC(ERROR_MONITOR_NOT_LOADED);
	}

	co_terminal_print("sweeping the kernel half of the address space\n");
	co_terminal_print("256 PML4 slots, 512 GB each, 128 TB total\n\n");
	co_terminal_print("  slots      address range                          state\n");
	co_terminal_print("  ---------  -------------------------------------  -----\n");

	/*
	 * Slots 256..511 are the kernel half. Bit 47 is set for all of them, so the
	 * canonical form needs bits 48..63 set as well.
	 */
	for (slot = 256; slot <= 512; slot++) {
		bool_t present = PFALSE;

		if (slot < 512) {
			co_manager_ioctl_probe_va_t probe = {0, };

			probe.va = 0xffff000000000000ULL |
				   ((unsigned long long)slot << 39);

			rc = co_manager_probe_va(handle, &probe);
			if (!CO_OK(rc)) {
				co_terminal_print("probe: ioctl failed at slot %d (rc %x)\n",
						  slot, (int)rc);
				co_os_manager_close(handle);
				return rc;
			}

			if (!probe.supported) {
				co_terminal_print("probe: not implemented for this architecture\n");
				co_os_manager_close(handle);
				return CO_RC(OK);
			}

			if (first_cr3 == 0)
				first_cr3 = probe.cr3;

			/*
			 * levels_walked == 1 with nothing present means the PML4 entry
			 * itself is absent -- the whole slot is unclaimed. Anything
			 * deeper means the host owns at least part of the range.
			 */
			present = (probe.entry[0] & 1) ? PTRUE : PFALSE;
			if (!present)
				free_slots++;
		}

		/* Run-length encode, so 256 rows collapse to a readable map. */
		if (run_start < 0) {
			run_start = slot;
			run_present = present;
			continue;
		}

		if (present != run_present || slot == 512) {
			unsigned long long lo = 0xffff000000000000ULL |
						((unsigned long long)run_start << 39);
			unsigned long long hi = (0xffff000000000000ULL |
						 ((unsigned long long)(slot - 1) << 39))
						+ ((1ULL << 39) - 1);
			int count = slot - run_start;

			if (count == 1)
				co_terminal_print("  %3d        %016llx-%016llx  %s\n",
						  run_start, lo, hi,
						  run_present ? "USED" : "free");
			else
				co_terminal_print("  %3d-%-3d    %016llx-%016llx  %s   (%d slots, %d TB)\n",
						  run_start, slot - 1, lo, hi,
						  run_present ? "USED" : "free",
						  count, (count * 512) / 1024);

			run_start = slot;
			run_present = present;
		}
	}

	co_os_manager_close(handle);

	co_terminal_print("\n  host cr3 0x%016llx\n", first_cr3);
	co_terminal_print("  %d of 256 kernel slots unclaimed = %d TB free\n",
			  free_slots, (free_slots * 512) / 1024);
	co_terminal_print("  guest window CO_VPTR_BASE 0x%016llx is slot %d\n",
			  (unsigned long long)CO_VPTR_BASE,
			  (int)((CO_VPTR_BASE >> 39) & 0x1ff));

	return CO_RC(OK);
}

co_rc_t co_winnt_probe_va(const char* arg)
{
	co_rc_t rc;
	bool_t installed = PFALSE;
	co_manager_handle_t handle;
	co_manager_ioctl_probe_va_t probe = {0, };
	unsigned long long va = CO_VPTR_BASE;
	int level;
	static const char* level_name[CO_PROBE_VA_LEVELS] = {
		"PML4", "PDPT", "PD  ", "PT  "
	};

	if (arg && arg[0]) {
		if (sscanf(arg, "%llx", &va) != 1 &&
		    sscanf(arg, "0x%llx", &va) != 1) {
			co_terminal_print("probe: cannot parse address '%s'\n", arg);
			return CO_RC(INVALID_PARAMETER);
		}
	}

	rc = co_win32_manager_is_installed(&installed);
	if (!CO_OK(rc))
		return rc;

	if (!installed) {
		co_terminal_print("driver not installed\n");
		return CO_RC(ERROR_ACCESSING_DRIVER);
	}

	handle = co_os_manager_open();
	if (!handle) {
		co_terminal_print("couldn't get driver handle\n");
		return CO_RC(ERROR_MONITOR_NOT_LOADED);
	}

	probe.va = va;
	rc = co_manager_probe_va(handle, &probe);
	co_os_manager_close(handle);

	if (!CO_OK(rc)) {
		co_terminal_print("probe: ioctl failed (rc %x)\n", (int)rc);
		return rc;
	}

	if (!probe.supported) {
		co_terminal_print("probe: not implemented for this architecture\n");
		return CO_RC(OK);
	}

	if (!CO_OK(probe.rc)) {
		co_terminal_print("probe: driver reported failure (rc %x)\n", (int)probe.rc);
		return probe.rc;
	}

	co_terminal_print("probing host mapping of 0x%016llx\n", va);
	co_terminal_print("  host cr3: 0x%016llx\n", probe.cr3);

	for (level = 0; level < probe.levels_walked; level++)
		co_terminal_print("  %s entry: 0x%016llx%s\n",
				  level_name[level], probe.entry[level],
				  (probe.entry[level] & 1) ? "" : "   (not present)");

	/*
	 * The level it stopped at is the whole point, and conflating the cases
	 * would be actively misleading. An absent PML4 entry means the host has
	 * nothing anywhere in that 512 GB slot, so the range can be claimed. An
	 * absent PTE means the opposite: the host owns the region and has page
	 * tables built over it, and merely has not filled this one page yet -- it
	 * could do so at any moment.
	 */
	if (probe.present) {
		co_terminal_print("  RESULT: mapped%s -- the host is using this address\n",
				  probe.large_page ? " (large page)" : "");
	} else {
		static const char* span[CO_PROBE_VA_LEVELS] = {
			"512 GB", "1 GB", "2 MB", "4 KB"
		};
		int level = probe.levels_walked - 1;

		co_terminal_print("  RESULT: not present at the %s level\n",
				  level_name[level]);

		if (level == 0)
			co_terminal_print("          the host has nothing in this entire %s slot,\n"
					  "          so the range is free to claim\n", span[level]);
		else
			co_terminal_print("          but the host owns the enclosing region -- it has page\n"
					  "          tables built down to this level, so only this %s is\n"
					  "          currently unmapped and it may be filled at any time.\n"
					  "          NOT safe to claim\n", span[level]);
	}

	return CO_RC(OK);
}

/*
 * Try each available page allocator and report whether its pages could host the
 * passage code. See CO_MANAGER_IOCTL_PROBE_PASSAGE in common/ioctl.h.
 *
 * Prints are kept short deliberately: co_terminal_print() has a fixed buffer and
 * silently truncated a long multi-line message the first time round.
 */
co_rc_t co_winnt_probe_passage(void)
{
	co_rc_t rc;
	bool_t installed = PFALSE;
	co_manager_handle_t handle;
	co_manager_ioctl_probe_passage_t probe = {0, };
	int i, usable = 0;
	static const char* level_name[CO_PROBE_VA_LEVELS] = { "PML4", "PDPT", "PD", "PT" };

	rc = co_win32_manager_is_installed(&installed);
	if (!CO_OK(rc))
		return rc;
	if (!installed) {
		co_terminal_print("driver not installed\n");
		return CO_RC(ERROR_ACCESSING_DRIVER);
	}

	handle = co_os_manager_open();
	if (!handle) {
		co_terminal_print("couldn't get driver handle\n");
		return CO_RC(ERROR_MONITOR_NOT_LOADED);
	}

	rc = co_manager_probe_passage(handle, &probe);
	co_os_manager_close(handle);

	if (!CO_OK(rc)) {
		co_terminal_print("probe: ioctl failed (rc %x)\n", (int)rc);
		return rc;
	}
	if (!CO_OK(probe.rc)) {
		co_terminal_print("probe: driver reported failure (rc %x)\n", (int)probe.rc);
		return probe.rc;
	}

	co_terminal_print("passage page is %d pages (%d bytes)\n",
			  probe.pages, probe.pages * 4096);
	co_terminal_print("the passage code executes from it, so it must be executable\n");
	co_terminal_print("\n");

	for (i = 0; i < probe.count; i++) {
		co_probe_alloc_result_t* r = &probe.result[i];
		unsigned long long pte;
		bool_t nx, uncached;

		co_terminal_print("[%d] %s%s\n", i, r->name,
				  r->used ? "   <== the passage page uses this" : "");

		if (!r->ok) {
			co_terminal_print("      allocation FAILED\n\n");
			continue;
		}

		co_terminal_print("      va 0x%016llx  pa 0x%016llx\n", r->va, r->pa);

		if (!r->walk.supported) {
			co_terminal_print("      (no page-table walker on this arch)\n\n");
			continue;
		}
		if (!r->walk.present) {
			co_terminal_print("      not mapped after %d levels -- unexpected\n\n",
					  r->walk.levels_walked);
			continue;
		}

		pte      = r->walk.entry[r->walk.levels_walked - 1];
		nx       = (pte & 0x8000000000000000ULL) ? PTRUE : PFALSE;
		uncached = (pte & 0x018) ? PTRUE : PFALSE;

		co_terminal_print("      %s pte 0x%016llx\n",
				  level_name[r->walk.levels_walked - 1], pte);
		co_terminal_print("      NX %s   caching %s   %s\n",
				  nx ? "SET    " : "clear  ",
				  uncached ? "uncached" : "cached  ",
				  (pte & 0x100) ? "global" : "");

		if (nx)
			co_terminal_print("      -> cannot execute from here\n");
		else if (uncached)
			co_terminal_print("      -> executable, but uncached: slow to run from\n");
		else
			co_terminal_print("      -> USABLE: executable and cached\n");

		if (!nx)
			usable++;

		co_terminal_print("\n");
	}

	if (usable)
		co_terminal_print("%d allocator(s) give executable pages, so no antinx\n", usable);
	else
		co_terminal_print("no allocator gives executable pages -- rethink needed\n");

	return CO_RC(OK);
}

/*
 * Show the host's CPU state as the switch would capture it. Reads only.
 */
co_rc_t co_winnt_save_state(bool_t restore)
{
	co_rc_t rc;
	bool_t installed = PFALSE;
	co_manager_handle_t handle;
	co_manager_ioctl_save_state_t r = {0, };
#if defined(__x86_64__)
	co_arch_state_stack_t* st = &r.state;
#endif

	rc = co_win32_manager_is_installed(&installed);
	if (!CO_OK(rc))
		return rc;
	if (!installed) {
		co_terminal_print("driver not installed\n");
		return CO_RC(ERROR_ACCESSING_DRIVER);
	}

	handle = co_os_manager_open();
	if (!handle) {
		co_terminal_print("couldn't get driver handle\n");
		return CO_RC(ERROR_MONITOR_NOT_LOADED);
	}

	r.restore = restore ? 1 : 0;
	if (restore)
		co_terminal_print("restoring the captured state in place -- if this returns,\n"
				  "the restore sequence survived\n\n");

	rc = co_manager_save_state(handle, &r);
	co_os_manager_close(handle);

	if (!CO_OK(rc)) {
		co_terminal_print("save-state: ioctl failed (rc %x)\n", (int)rc);
		return rc;
	}
	if (!r.supported) {
		co_terminal_print("save-state: not implemented on this architecture\n");
		return CO_RC(OK);
	}

#if !defined(__x86_64__)
	/*
	 * The long-mode fields below (EFER, the SYSCALL MSRs, the segment-base
	 * MSRs) do not exist in the 32-bit co_arch_state_stack_t, so this whole
	 * report is x86-64 only. The driver reports unsupported there anyway.
	 */
	co_terminal_print("save-state: x86-64 only\n");
	return CO_RC(OK);
#else
	co_terminal_print("host CPU state as the switch would capture it\n");
	co_terminal_print("\n");
	co_terminal_print("  cs %04llx  ds %04llx  es %04llx\n", st->cs, st->ds, st->es);
	co_terminal_print("  fs %04llx  gs %04llx  ss %04llx\n", st->fs, st->gs, st->ss);
	co_terminal_print("  ldt %04x  tr %04x\n", st->ldt, st->tr);
	co_terminal_print("\n");
	co_terminal_print("  cr0 0x%016llx  cr2 0x%016llx\n", st->cr0, st->cr2);
	co_terminal_print("  cr3 0x%016llx  cr4 0x%016llx\n", st->cr3, st->cr4);
	co_terminal_print("\n");
	co_terminal_print("  gdt base 0x%016llx limit %04x\n",
			  (unsigned long long)(size_t)st->gdt.base, st->gdt.limit);
	co_terminal_print("  idt base 0x%016llx limit %04x\n",
			  (unsigned long long)(size_t)st->idt.table, st->idt.size);
	co_terminal_print("\n");
	co_terminal_print("  efer   0x%016llx  %s%s%s%s\n", st->efer,
			  (st->efer & 0x001) ? "SCE " : "",
			  (st->efer & 0x100) ? "LME " : "",
			  (st->efer & 0x400) ? "LMA " : "",
			  (st->efer & 0x800) ? "NXE " : "");
	co_terminal_print("  star   0x%016llx\n", st->star);
	co_terminal_print("  lstar  0x%016llx\n", st->lstar);
	co_terminal_print("  cstar  0x%016llx\n", st->cstar);
	co_terminal_print("  sfmask 0x%016llx\n", st->sfmask);
	co_terminal_print("\n");
	co_terminal_print("  fs_base        0x%016llx\n", st->fs_base);
	co_terminal_print("  gs_base        0x%016llx\n", st->gs_base);
	co_terminal_print("  kernel_gs_base 0x%016llx\n", st->kernel_gs_base);
	co_terminal_print("\n");
	co_terminal_print("  sysenter cs 0x%llx esp 0x%llx eip 0x%llx\n",
			  st->sysenter_cs, st->sysenter_esp, st->sysenter_eip);
	co_terminal_print("  dr7 0x%016llx  dr6 0x%016llx\n", st->dr7, st->dr6);

	if (restore)
		co_terminal_print("\n  RESTORE SURVIVED -- ordering and TSS busy-bit handling are right\n");

	return CO_RC(OK);
#endif
}

/*
 * Change CR3 into a minimal guest address space and come back. See
 * CO_MANAGER_IOCTL_TEST_SWITCH.
 */
co_rc_t co_winnt_test_switch(int mode)
{
	co_rc_t rc;
	bool_t installed = PFALSE;
	co_manager_handle_t handle;
	co_manager_ioctl_test_switch_t r = {0, };

	rc = co_win32_manager_is_installed(&installed);
	if (!CO_OK(rc))
		return rc;
	if (!installed) {
		co_terminal_print("driver not installed\n");
		return CO_RC(ERROR_ACCESSING_DRIVER);
	}

	handle = co_os_manager_open();
	if (!handle) {
		co_terminal_print("couldn't get driver handle\n");
		return CO_RC(ERROR_MONITOR_NOT_LOADED);
	}

	if (mode == 6 || mode == 7) {
		co_terminal_print("running a guest whose code and stack are outside the passage\n");
		co_terminal_print("page, in an address space built a page at a time%s\n",
				  (mode == 7) ? " -- and faulting there" : "");
	} else if (mode == 5) {
		co_terminal_print("destroying the guest stack pointer and then faulting --\n");
		co_terminal_print("only an IST stack can deliver that\n");
	} else if (mode == 4) {
		co_terminal_print("reading an address the guest maps nothing at, and expecting\n");
		co_terminal_print("its own IDT to report which fault, where, and why\n");
	} else if (mode == 3) {
		/* input field, read by the driver before it clears the struct */
		r.iterations = 64;
		co_terminal_print("entering the guest %d times over, requiring it to continue\n",
				  r.iterations);
		co_terminal_print("from where it stopped rather than restarting\n");
	} else if (mode == 2) {
		co_terminal_print("entering the guest address space, raising #UD there, and\n");
		co_terminal_print("expecting the guest's own IDT to hand control back cleanly\n");
	} else if (mode == 1) {
		co_terminal_print("entering the guest address space by far return, running\n");
		co_terminal_print("code there, and coming back the same way\n");
	} else {
		co_terminal_print("switching CR3 into an address space that maps one page,\n");
		co_terminal_print("storing a sentinel from code executing there, switching back\n");
	}
	co_terminal_print("\n");

	rc = co_manager_test_switch(handle, &r, mode);
	co_os_manager_close(handle);

	if (!CO_OK(rc)) {
		co_terminal_print("switch test: ioctl failed (rc %x)\n", (int)rc);
		return rc;
	}
	if (r.preflight_failed) {
		co_terminal_print("  PREFLIGHT REFUSED THE ENTRY.\n");
		co_terminal_print("  0x%016llx is not usable in the guest address space\n", r.preflight_va);
		co_terminal_print("  (absent at level %d, 0 = PML4). CR3 was never loaded.\n", r.preflight_level);
		return CO_RC(OK);
	}
	if (!r.supported) {
		co_terminal_print("switch test: not implemented on this architecture\n");
		return CO_RC(OK);
	}
	if (!CO_OK(r.rc)) {
		co_terminal_print("switch test: driver reported failure (rc %x)\n", (int)r.rc);
		return r.rc;
	}

	co_terminal_print("  passage page va 0x%016llx  pa 0x%016llx\n",
			  r.passage_va, r.passage_pa);
	co_terminal_print("  code va         0x%016llx  (%lu bytes)\n",
			  r.code_va, r.code_size);
	co_terminal_print("  host  cr3       0x%016llx\n", r.host_cr3);
	co_terminal_print("  guest cr3       0x%016llx\n", r.guest_cr3);
	if (r.guest_gdt)
		co_terminal_print("  guest gdt       0x%016llx  (in the passage page)\n", r.guest_gdt);
	if (r.guest_idt) {
		co_terminal_print("  guest idt       0x%016llx  (256 gates)\n", r.guest_idt);
		co_terminal_print("  vector stubs    0x%016llx  (256 x 16 bytes)\n", r.guest_stubs);
		co_terminal_print("  guest tss       0x%016llx  ist1 0x%016llx\n", r.guest_tss, r.ist_stack);
		co_terminal_print("  fault handler   0x%016llx\n", r.fault_handler);
	}
	if (r.guest_text)
		co_terminal_print("  guest text      0x%016llx  stack 0x%016llx  (%lu table pages)\n",
				  r.guest_text, r.guest_stack, r.tables);
	if (r.preflight_checked)
		co_terminal_print("  preflight       %d addresses resolved in the guest tables\n",
				  r.preflight_checked);
	co_terminal_print("\n");
	if (mode == 3) {
		co_terminal_print("  entries requested %d\n", r.iterations);
		co_terminal_print("  guest counter     %llu   (memory in the passage page)\n",
				  r.counter);
		co_terminal_print("  guest rbx total   %llu   (callee-saved across each crossing)\n",
				  r.reg_accum);
	} else {
		co_terminal_print("  sentinel expected 0x%016llx\n", r.expected);
		co_terminal_print("  sentinel observed 0x%016llx\n", r.observed);
	}
	co_terminal_print("\n");

	if (r.faulted) {
		static const struct { int v; const char* name; } names[] = {
			{ 0, "#DE divide error" },   { 1, "#DB debug" },
			{ 3, "#BP breakpoint" },     { 4, "#OF overflow" },
			{ 6, "#UD invalid opcode" }, { 8, "#DF double fault" },
			{ 11, "#NP segment not present" }, { 12, "#SS stack fault" },
			{ 13, "#GP general protection" }, { 14, "#PF page fault" },
			{ -1, NULL }
		};
		const char* name = "unknown";
		int i;

		for (i = 0; names[i].name; i++)
			if (names[i].v == (int)r.vector)
				name = names[i].name;

		co_terminal_print("  FAULT: vector %llu -- %s\n", r.vector, name);
		co_terminal_print("    at rip      0x%016llx\n", r.fault_rip);
		co_terminal_print("    error code  0x%016llx\n", r.error_code);
		if (r.vector == 14) {
			co_terminal_print("    cr2         0x%016llx  (the address that faulted)\n", r.cr2);
			co_terminal_print("    decoded     %s, %s, %s\n",
					  (r.error_code & 1) ? "protection violation" : "page not present",
					  (r.error_code & 2) ? "write" : "read",
					  (r.error_code & 4) ? "user mode" : "supervisor mode");
		}
		co_terminal_print("\n");
	}

	if (r.succeeded) {
		if (mode == 7) {
			co_terminal_print("  FAULTED OUTSIDE THE PASSAGE PAGE, AND CAME BACK. The guest\n");
			co_terminal_print("  executed at 0x%016llx, faulted there, and its stubs -- which\n", r.guest_text);
			co_terminal_print("  do live in the passage page -- still delivered and reported it.\n");
		} else if (mode == 6) {
			co_terminal_print("  RAN OUTSIDE THE PASSAGE PAGE. Instruction fetch after the CR3\n");
			co_terminal_print("  write landed at 0x%016llx, in a page mapped only in the guest,\n", r.guest_text);
			co_terminal_print("  on a stack of its own -- and it found its way back.\n");
		} else if (mode == 5) {
			if (r.faulted)
				co_terminal_print("  IST WORKED. The guest faulted with an unusable stack pointer\n"
						  "  and the fault was still delivered -- the CPU took the stack\n"
						  "  from the TSS before pushing anything. Without IST this is a\n"
						  "  double fault and a reset.\n");
			else
				co_terminal_print("  came back without recording a fault\n");
		} else if (mode == 4) {
			if (r.vector == 14)
				co_terminal_print("  CAUGHT AND IDENTIFIED. The guest page faulted, its own stub\n"
						  "  recorded which vector, the error code and CR2, and the handler\n"
						  "  switched back -- no reset, and enough to say what went wrong.\n");
			else
				co_terminal_print("  came back, but the fault was vector %llu rather than #PF\n",
						  r.vector);
		} else if (mode == 3) {
			co_terminal_print("  RESUMED. The guest was entered %d times and continued from\n",
					  r.iterations);
			co_terminal_print("  where it stopped each time -- the first entry landed at the\n");
			co_terminal_print("  top of its loop, every later one resumed inside the switch.\n");
			co_terminal_print("  Its callee-saved registers survived every crossing.\n");
		} else if (mode == 2) {
			if (r.faulted) {
				co_terminal_print("  FAULT CAUGHT. The guest raised #UD at 0x%016llx, its own\n", r.fault_rip);
				co_terminal_print("  IDT vectored to the handler in the passage page, and the\n");
				co_terminal_print("  handler switched back instead of triple faulting.\n");
			} else {
				co_terminal_print("  came back, but no fault was recorded -- the ud2 did not\n");
				co_terminal_print("  execute, or the flag was not written\n");
			}
		} else if (mode == 1) {
			co_terminal_print("  ROUND TRIP COMPLETE. Control crossed into the guest address\n");
			co_terminal_print("  space by far return, ran there, and came back -- so CS was\n");
			co_terminal_print("  reloaded correctly in both directions.\n");
		} else {
			co_terminal_print("  SWITCHED. Execution continued across the CR3 write and the\n");
			co_terminal_print("  store landed, so the passage page is reachable at the same\n");
			co_terminal_print("  address in both address spaces.\n");
		}
	} else if (mode == 3) {
		if (r.faulted)
			co_terminal_print("  the guest FAULTED at 0x%016llx while looping\n", r.fault_rip);
		else if (r.counter != (unsigned long long)r.iterations)
			co_terminal_print("  the guest did NOT resume: counted %llu of %d entries -- it is\n"
					  "  restarting, or never came back past the first crossing\n",
					  r.counter, r.iterations);
		else
			co_terminal_print("  the guest resumed (counter %llu) but its registers did NOT\n"
					  "  survive: rbx totalled %llu, expected %d\n",
					  r.counter, r.reg_accum, r.iterations);
	} else {
		co_terminal_print("  returned, but the sentinel is wrong -- the store did not land\n");
	}

	return CO_RC(OK);
}

/*
 * The SMP spike: two threads, two processors, two guests, at the same time.
 *
 * Each lane is a thread of this process holding its own driver handle inside
 * a blocking TEST_SMP ioctl -- the exact shape N vCPUs will have, minus the
 * Linux. The driver pins lane N to the Nth active processor (processor masks
 * may be sparse) and refuses lanes the host does not have, so running this on
 * a uniprocessor box reports rather than pretends.
 */
struct co_smp_lane_ctx {
	co_manager_ioctl_test_smp_t r;
	co_rc_t rc;
};

static void co_winnt_smp_lane_thread(void* arg)
{
	struct co_smp_lane_ctx* ctx = arg;
	co_manager_handle_t handle;

	handle = co_os_manager_open();
	if (!handle) {
		ctx->rc = CO_RC(ERROR_ACCESSING_DRIVER);
		return;
	}

	ctx->rc = co_manager_test_smp(handle, &ctx->r);
	co_os_manager_close(handle);
}

co_rc_t co_winnt_test_smp(const char* iterations_arg)
{
	co_rc_t rc;
	bool_t installed = PFALSE;
	struct co_smp_lane_ctx lane[2];
	void* thread[2];
	long long iterations;
	int i, ok;

	rc = co_win32_manager_is_installed(&installed);
	if (!CO_OK(rc))
		return rc;
	if (!installed) {
		co_terminal_print("driver not installed\n");
		return CO_RC(ERROR_ACCESSING_DRIVER);
	}

	iterations = (iterations_arg && iterations_arg[0])
		? (long long)strtoul(iterations_arg, NULL, 0) : 2000000LL;

	co_terminal_print("two threads, each pinned to its own processor, each crossing\n");
	co_terminal_print("into a guest of its own %lld times over -- concurrently, with\n",
			  iterations);
	co_terminal_print("lane-distinct MSR sentinels that must never meet\n\n");

	for (i = 0; i < 2; i++) {
		memset(&lane[i], 0, sizeof(lane[i]));
		lane[i].r.lane       = i;
		lane[i].r.iterations = iterations;
		lane[i].rc           = CO_RC(ERROR);
	}

	thread[0] = co_os_thread_start(co_winnt_smp_lane_thread, &lane[0]);
	thread[1] = co_os_thread_start(co_winnt_smp_lane_thread, &lane[1]);
	if (!thread[0] || !thread[1]) {
		co_terminal_print("could not start the lane threads\n");
		if (thread[0]) co_os_thread_join(thread[0]);
		if (thread[1]) co_os_thread_join(thread[1]);
		return CO_RC(ERROR);
	}

	co_os_thread_join(thread[0]);
	co_os_thread_join(thread[1]);

	ok = 1;
	for (i = 0; i < 2; i++) {
		co_manager_ioctl_test_smp_t* r = &lane[i].r;

		co_terminal_print("lane %d (processor %lu):\n", i, r->host_cpu);

		if (!CO_OK(lane[i].rc)) {
			co_terminal_print("  ioctl failed (rc %x)\n", (int)lane[i].rc);
			ok = 0;
			continue;
		}
		if (!CO_OK(r->rc)) {
			co_terminal_print("  driver refused the lane (rc %x) -- a "
					  "uniprocessor host reports here\n", (int)r->rc);
			ok = 0;
			continue;
		}
		if (!r->supported) {
			co_terminal_print("  not implemented on this architecture\n");
			ok = 0;
			continue;
		}
		if (r->preflight_failed) {
			co_terminal_print("  PREFLIGHT REFUSED THE ENTRY at 0x%016llx "
					  "(level %d)\n", r->preflight_va,
					  r->preflight_level);
			ok = 0;
			continue;
		}

		co_terminal_print("  crossings  %lld of %lld  (counter %llu, rbx %llu)\n",
				  r->completed, r->iterations, r->counter, r->reg_accum);
		co_terminal_print("  replayed   %lld host interrupts from inside the guest\n",
				  r->interrupts);

		if (r->faulted) {
			co_terminal_print("  FAULTED: vector %llu at rip 0x%016llx, "
					  "err 0x%llx\n", r->vector, r->fault_rip,
					  r->error_code);
			ok = 0;
		}
		if (r->migrated) {
			co_terminal_print("  MIGRATED off its processor mid-run -- "
					  "the pin did not hold\n");
			ok = 0;
		}
		if (r->unforwardable) {
			co_terminal_print("  hit an unforwardable host vector %llu\n",
					  r->vector);
			ok = 0;
		}
		if (r->aborted) {
			co_terminal_print("  aborted early (KSTOP or driver unload)\n");
			ok = 0;
		}
		if (!r->msr_ok) {
			co_terminal_print("  MSR STATE CROSSED LANES: msr 0x%lx was "
					  "0x%016llx, came back 0x%016llx\n",
					  r->msr_bad, r->msr_want, r->msr_got);
			ok = 0;
		}
		if (!r->succeeded)
			ok = 0;
		co_terminal_print("\n");
	}

	if (ok) {
		co_terminal_print("  CONCURRENT CROSSINGS HELD. Two processors switched worlds\n");
		co_terminal_print("  independently -- descriptor tables, MSRs and FPU state stayed\n");
		co_terminal_print("  per-lane throughout, and host interrupts were replayed from\n");
		co_terminal_print("  both sides at once. The ground the SMP work stands on.\n");
	} else {
		co_terminal_print("  THE LANES DID NOT BOTH SURVIVE INTACT -- see above. Fix this\n");
		co_terminal_print("  before any per-vCPU work builds on it.\n");
	}

	return ok ? CO_RC(OK) : CO_RC(ERROR);
}

/*
 * Run a secondary vCPU alongside whatever else the driver is doing.
 *
 * Unlike --test-smp, this is meant to be issued WHILE a guest is booting on
 * vCPU 0: it is the first time the real monitor loop and a second guest
 * processor exist at the same time. The driver picks a host core no other
 * vCPU holds, so it cannot land on the boot processor's -- two vCPUs on one
 * core is a deadlock, not a slowdown.
 *
 * The payload is still the trivial crossing loop. Entering the kernel needs
 * an initial state only the guest can supply, which is the next rung.
 */
co_rc_t co_winnt_test_vcpu(const char* arg)
{
	co_rc_t rc;
	bool_t installed = PFALSE;
	co_manager_handle_t handle;
	co_manager_ioctl_kvcpu_run_t r;

	rc = co_win32_manager_is_installed(&installed);
	if (!CO_OK(rc))
		return rc;
	if (!installed) {
		co_terminal_print("driver not installed\n");
		return CO_RC(ERROR_ACCESSING_DRIVER);
	}

	handle = co_os_manager_open();
	if (!handle) {
		co_terminal_print("couldn't get driver handle\n");
		return CO_RC(ERROR_MONITOR_NOT_LOADED);
	}

	memset(&r, 0, sizeof(r));
	r.vcpu       = 1;
	r.iterations = (arg && arg[0]) ? (long long)strtoul(arg, NULL, 0) : 2000000LL;

	co_terminal_print("running vcpu %d for %lld crossings, on whichever host\n",
			  r.vcpu, r.iterations);
	co_terminal_print("processor no other vCPU is holding\n\n");

	rc = co_manager_kvcpu_run(handle, &r);
	co_os_manager_close(handle);

	if (!CO_OK(rc)) {
		co_terminal_print("vcpu run: ioctl failed (rc %x)\n", (int)rc);
		return rc;
	}
	if (r.no_free_core) {
		co_terminal_print("  EVERY HOST PROCESSOR ALREADY CARRIES A vCPU.\n");
		co_terminal_print("  Nothing was started -- two vCPUs on one core deadlock.\n");
		return CO_RC(ERROR);
	}
	if (!CO_OK(r.rc)) {
		co_terminal_print("vcpu run: driver refused (rc %x)\n", (int)r.rc);
		return r.rc;
	}
	if (!r.supported) {
		co_terminal_print("  not implemented on this architecture\n");
		return CO_RC(OK);
	}

	co_terminal_print("vcpu %d (host processor %lu):\n", r.vcpu, r.host_cpu);
	co_terminal_print("  crossings  %lld of %lld  (counter %llu, rbx %llu)\n",
			  r.completed, r.iterations, r.counter, r.reg_accum);
	co_terminal_print("  replayed   %lld host interrupts from inside the guest\n",
			  r.interrupts);

	if (r.faulted)
		co_terminal_print("  FAULTED: vector %llu at rip 0x%016llx\n",
				  r.vector, r.fault_rip);
	if (r.migrated)
		co_terminal_print("  MIGRATED off its processor -- the pin did not hold\n");
	if (!r.msr_ok)
		co_terminal_print("  MSR STATE CROSSED: msr 0x%lx wanted 0x%016llx, got 0x%016llx\n",
				  r.msr_bad, r.msr_want, r.msr_got);
	if (r.aborted)
		co_terminal_print("  aborted early (KSTOP or driver unload)\n");

	co_terminal_print("\n");
	if (r.succeeded) {
		co_terminal_print("  A SECOND vCPU RAN. It took a core of its own, crossed\n");
		co_terminal_print("  and returned without disturbing whatever vCPU 0 was\n");
		co_terminal_print("  doing, and its saved state stayed its own throughout.\n");
	} else {
		co_terminal_print("  THE vCPU DID NOT COMPLETE INTACT -- see above.\n");
	}

	return r.succeeded ? CO_RC(OK) : CO_RC(ERROR);
}

static co_rc_t co_winnt_install_driver_lowlevel(IN SC_HANDLE SchSCManager, IN LPCTSTR  DriverName, IN LPCTSTR ServiceExe)
{
	SC_HANDLE  schService;

	/*
	 * SERVICE_DEMAND_START, not SERVICE_AUTO_START.
	 *
	 * An auto-start kernel driver loads on every boot, so a driver that
	 * bugchecks turns the machine into a boot loop: it dies during service
	 * startup, every time, before a desktop exists to uninstall it from.
	 * Recovering means Last Known Good, Safe Mode or a rescue disk.
	 *
	 * Nothing needs auto-start. co_winnt_load_driver_lowlevel_by_name() starts
	 * the service explicitly right after creating it, and the daemon loads the
	 * driver on demand, so demand-start behaves identically in use while
	 * leaving a failed driver harmless across a reboot.
	 */
	schService = CreateService(SchSCManager,
				   DriverName,
				   DriverName,
				   SERVICE_ALL_ACCESS,
				   SERVICE_KERNEL_DRIVER,
				   SERVICE_DEMAND_START,
				   SERVICE_ERROR_NORMAL,
				   ServiceExe,
				   NULL,
				   NULL,
				   NULL,
				   NULL,
				   NULL);

	if (schService == NULL) {
		co_terminal_print_last_error("Install lowlevel driver failed");
		return CO_RC(ERROR_INSTALLING_DRIVER);
	}

	/* Possible error: ERROR_SERVICE_EXISTS */

	CloseServiceHandle(schService);

	return CO_RC(OK);
}

static co_rc_t co_winnt_remove_driver_lowlevel(IN SC_HANDLE  SchSCManager, IN LPCTSTR DriverName)
{
	SC_HANDLE  schService;
	co_rc_t   rc;

	schService = OpenService (SchSCManager,
				  DriverName,
				  SERVICE_ALL_ACCESS);

	if (schService == NULL)
		return CO_RC(ERROR);

	if (DeleteService(schService))
		rc = CO_RC(OK);
	else
		rc = CO_RC(ERROR_REMOVING_DRIVER);

	CloseServiceHandle(schService);

	return rc;
}

static co_rc_t co_winnt_start_driver_lowlevel(IN SC_HANDLE SchSCManager, IN LPCTSTR DriverName)
{
	SC_HANDLE  schService;
	co_rc_t   ret;
	DWORD      err;

	schService = OpenService(SchSCManager, DriverName, SERVICE_ALL_ACCESS);

	if (schService == NULL) {
		return CO_RC(ERROR_ACCESSING_DRIVER);
	}

	if (StartService(schService, 0, NULL)) {
		ret = CO_RC(OK);
	} else {
		ret = CO_RC(ERROR_STARTING_DRIVER);

		err = GetLastError();
#if 1
		if (err == ERROR_SERVICE_ALREADY_RUNNING)
			co_terminal_print("failure: StartService, ERROR_SERVICE_ALREADY_RUNNING\n");
		else
			co_terminal_print("failure: StartService (0x%lx)\n", err);
#endif
	}

	CloseServiceHandle(schService);

	return ret;
}

static co_rc_t co_winnt_stop_driver_lowlevel(IN SC_HANDLE  SchSCManager, IN LPCTSTR DriverName)
{
	SC_HANDLE       schService;
	SERVICE_STATUS  serviceStatus;
	co_rc_t        rc;

	schService = OpenService(SchSCManager, DriverName, SERVICE_ALL_ACCESS);

	if (schService == NULL)
		return CO_RC(ERROR_ACCESSING_DRIVER);

	if (ControlService(schService, SERVICE_CONTROL_STOP, &serviceStatus))
		rc = CO_RC(OK);
	else {
		rc = CO_RC(ERROR_STOPPING_DRIVER);
	}

	CloseServiceHandle(schService);

	return rc;
}

static co_rc_t co_winnt_unload_driver_lowlevel_by_name(char *name)
{
	SC_HANDLE   schSCManager;
	co_rc_t rc;

	co_debug_end();

	schSCManager = OpenSCManager (NULL,                 // machine (NULL == local)
				      NULL,                 // database (NULL == default)
				      SC_MANAGER_ALL_ACCESS // access required
				     );
	if (schSCManager == NULL)
		return CO_RC(ERROR_ACCESSING_DRIVER);

	co_debug("stopping driver service");
	rc = co_winnt_stop_driver_lowlevel(schSCManager, name);

	co_debug("removing driver service");
	rc = co_winnt_remove_driver_lowlevel(schSCManager, name);

	CloseServiceHandle(schSCManager);

	/*
	 * Apparently this givens the service manager an opportunity to
	 * remove the service before we reinstall it.
	 */
	Sleep(100);

	return rc;
}

static co_rc_t co_winnt_load_driver_lowlevel_by_name(char *name, char *path)
{
	SC_HANDLE   schSCManager;
	char fullpath[0x100] = {0,};
	char driverfullpath[0x100] = {0,};
	co_rc_t rc;

	GetModuleFileName(NULL, fullpath, sizeof(fullpath));
	PathRemoveFileSpec(fullpath);
	PathCombine(driverfullpath, fullpath, path);

	co_terminal_print("loading %s\n", driverfullpath);

	schSCManager = OpenSCManager(NULL,                 // machine (NULL == local)
				     NULL,                 // database (NULL == default)
				     SC_MANAGER_ALL_ACCESS // access required
				     );
	if (schSCManager == NULL)
		return CO_RC(ERROR_ACCESSING_DRIVER);

	rc = co_winnt_install_driver_lowlevel(schSCManager, name, driverfullpath);
	if (!CO_OK(rc)) {
		CloseServiceHandle(schSCManager);
		return rc;
	}

	rc = co_winnt_start_driver_lowlevel(schSCManager, name);
	if (!CO_OK(rc)) {
		co_winnt_remove_driver_lowlevel(schSCManager, name);
		CloseServiceHandle(schSCManager);
		return rc;
	}

#if (0)
	rc = co_os_check_device(name);
	if (!CO_OK(rc)) {
		co_winnt_stop_driver_lowlevel(schSCManager, name);
		co_winnt_remove_driver_lowlevel(schSCManager, name);
		CloseServiceHandle(schSCManager);
		return rc;
	}
#endif

	CloseServiceHandle(schSCManager);

	return CO_RC(OK);
}

static co_rc_t co_winnt_driver_remove_lowlevel(void)
{
	return co_winnt_unload_driver_lowlevel_by_name(CO_DRIVER_NAME);
}

static co_rc_t co_winnt_driver_install_lowlevel(void)
{
	return co_winnt_load_driver_lowlevel_by_name(CO_DRIVER_NAME, COLINUX_DRIVER_FILE);
}

/*
 * Build a guest address space and verify it without ever entering it.
 */
co_rc_t co_winnt_test_space(void)
{
	co_rc_t rc;
	bool_t installed = PFALSE;
	co_manager_handle_t handle;
	co_manager_ioctl_test_space_t r = {0, };

	rc = co_win32_manager_is_installed(&installed);
	if (!CO_OK(rc))
		return rc;
	if (!installed) {
		co_terminal_print("driver not installed\n");
		return CO_RC(ERROR_ACCESSING_DRIVER);
	}

	handle = co_os_manager_open();
	if (!handle) {
		co_terminal_print("couldn't get driver handle\n");
		return CO_RC(ERROR_MONITOR_NOT_LOADED);
	}

	co_terminal_print("building a four-level guest address space and reading it\n");
	co_terminal_print("back, without ever loading it into CR3\n\n");

	rc = co_manager_test_space(handle, &r);
	co_os_manager_close(handle);

	if (!CO_OK(rc)) {
		co_terminal_print("space test: ioctl failed (rc %x)\n", (int)rc);
		return rc;
	}
	if (!r.supported) {
		co_terminal_print("space test: not implemented on this architecture\n");
		return CO_RC(OK);
	}
	if (!CO_OK(r.rc)) {
		co_terminal_print("space test: driver reported failure (rc %x)\n", (int)r.rc);
		return r.rc;
	}

	co_terminal_print("  guest cr3 would be 0x%016llx\n", r.root);
	co_terminal_print("  page-table pages   %lu\n", r.tables);
	co_terminal_print("\n");
	co_terminal_print("  pages mapped       %lu\n", r.mapped);
	co_terminal_print("  verified by walk   %lu\n", r.verified);
	co_terminal_print("  mismatched         %lu\n", r.mismatched);
	co_terminal_print("  unmapped address   %s (missing at level %d)\n",
			  r.unmapped_reported ? "correctly reported absent" : "WRONGLY REPORTED PRESENT",
			  r.unmapped_level);
	co_terminal_print("\n");

	if (r.succeeded) {
		co_terminal_print("  SPACE BUILT. Three regions in three different PML4 slots,\n");
		co_terminal_print("  every page resolving to the physical address it was given,\n");
		co_terminal_print("  and an unmapped address correctly reported as absent.\n");
	} else if (r.mismatched) {
		co_terminal_print("  MISMATCH at 0x%016llx: expected pa 0x%016llx, walk found 0x%016llx\n",
				  r.first_bad_va, r.first_bad_expect, r.first_bad_got);
	} else {
		co_terminal_print("  the walker reported a never-mapped address as present\n");
	}

	return CO_RC(OK);
}
