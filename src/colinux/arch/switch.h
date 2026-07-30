/*
 * This source code is a part of coLinux source package.
 *
 * The code is licensed under the GPL. See the COPYING file at
 * the root directory.
 */

#ifndef __COLINUX_ARCH_SWITCH_H__
#define __COLINUX_ARCH_SWITCH_H__

#include <colinux/common/common.h>
#include <colinux/kernel/manager.h>

/*
 * Result of changing CR3 into a minimal guest address space and continuing to
 * execute there. See arch/x86_64/switch.c.
 */
typedef struct {
	int		   supported;
	int		   succeeded;
	unsigned long long passage_va;
	unsigned long long passage_pa;
	unsigned long long code_va;
	unsigned long long host_cr3;
	unsigned long long guest_cr3;
	unsigned long long expected;
	unsigned long long observed;
	unsigned long long guest_gdt;
	unsigned long long guest_idt;
	unsigned long long fault_handler;
	unsigned long long fault_rip;
	int		   faulted;
	unsigned long	   code_size;
	/* resume test: how many times the guest was entered, and what it counted */
	int		   iterations;
	unsigned long long counter;
	unsigned long long reg_accum;
	/* what the fault stubs recorded */
	unsigned long long guest_stubs;
	unsigned long long vector;
	unsigned long long error_code;
	unsigned long long cr2;
	/* the pre-flight walk of the guest tables, before CR3 was ever loaded */
	int		   preflight_checked;
	int		   preflight_failed;
	int		   preflight_level;
	unsigned long long preflight_va;
} co_arch_switch_test_t;

extern co_rc_t co_arch_test_switch(co_manager_t* manager, co_arch_switch_test_t* out);

/* Enter the guest address space, run code there, and come back. */
extern co_rc_t co_arch_test_roundtrip(co_manager_t* manager, co_arch_switch_test_t* out,
				      int provoke_fault);

/* Enter the guest N times and require it to continue from where it stopped. */
extern co_rc_t co_arch_test_resume(co_manager_t* manager, co_arch_switch_test_t* out,
				   int iterations);

#endif
