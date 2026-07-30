/*
 * This source code is a part of coLinux source package.
 *
 * The code is licensed under the GPL. See the COPYING file at
 * the root directory.
 */

/*
 * Capture the current CPU state into a co_arch_state_stack_t.
 *
 * This is the save half of the world switch, and nothing else. It reads: no
 * control register, descriptor table or MSR is written, so it cannot disturb the
 * host. Run it before writing the restore half, because it answers two things
 * that are otherwise guesses:
 *
 *   - whether co_arch_state_stack_t's layout is right. The offsets are asserted
 *     against the struct at build time, but nothing has yet checked that the
 *     fields hold what their names claim on real hardware.
 *
 *   - what Windows' actual state looks like. The restore half has to put all of
 *     it back exactly, and the values matter: whether EFER.SCE is set decides
 *     whether the SYSCALL MSRs are live, whether KERNEL_GS_BASE is non-zero says
 *     how the host uses swapgs, and TR must be reloaded with its busy bit
 *     cleared or the ltr faults.
 *
 * Written in C with inline asm rather than as a position-independent blob. The
 * blob machinery only matters once code has to keep executing across a CR3
 * change; for a pure read there is no reason to take on that complexity.
 */

#include <colinux/common/common.h>
#include <colinux/common/debug.h>
#include <colinux/common/libc.h>
#include <colinux/kernel/manager.h>
#include <colinux/arch/state.h>

#include "utils.h"
#include "defs.h"

void co_arch_save_state(co_arch_state_stack_t* state)
{
	unsigned short sel;

	co_memset(state, 0, sizeof(*state));

	/*
	 * Segment selectors. In long mode these carry no base or limit -- the
	 * bases for FS and GS live in MSRs, read below -- but CS still selects the
	 * code segment's mode and CPL, and SS is reloaded on the way back.
	 */
	asm volatile("mov %%cs, %0" : "=r"(sel)); state->cs = sel;
	asm volatile("mov %%ds, %0" : "=r"(sel)); state->ds = sel;
	asm volatile("mov %%es, %0" : "=r"(sel)); state->es = sel;
	asm volatile("mov %%fs, %0" : "=r"(sel)); state->fs = sel;
	asm volatile("mov %%gs, %0" : "=r"(sel)); state->gs = sel;
	asm volatile("mov %%ss, %0" : "=r"(sel)); state->ss = sel;

	state->cr0 = co_get_cr0();
	state->cr2 = co_get_cr2();
	state->cr3 = co_get_cr3();
	state->cr4 = co_get_cr4();

	/*
	 * None of these exist in the i386 state. EFER carries long mode itself;
	 * the segment-base MSRs are what a selector reload does *not* restore, and
	 * Windows keeps the KPCR at GS base, so losing them kills the host.
	 */
	state->efer           = co_read_msr(CO_MSR_IA32_EFER);
	state->star           = co_read_msr(CO_MSR_IA32_STAR);
	state->lstar          = co_read_msr(CO_MSR_IA32_LSTAR);
	state->cstar          = co_read_msr(CO_MSR_IA32_CSTAR);
	state->sfmask         = co_read_msr(CO_MSR_IA32_SFMASK);
	state->fs_base        = co_read_msr(CO_MSR_IA32_FS_BASE);
	state->gs_base        = co_read_msr(CO_MSR_IA32_GS_BASE);
	state->kernel_gs_base = co_read_msr(CO_MSR_IA32_KERNEL_GS_BASE);

	/* Still present in long mode, and used by WOW64's 32-bit syscall path. */
	state->sysenter_cs  = co_read_msr(0x174);
	state->sysenter_esp = co_read_msr(0x175);
	state->sysenter_eip = co_read_msr(0x176);

	/* 10 bytes each in long mode, against 6 on i386. */
	asm volatile("sgdt %0" : "=m"(state->gdt));
	asm volatile("sidt %0" : "=m"(state->idt));
	asm volatile("sldt %0" : "=m"(state->ldt));
	asm volatile("str %0"  : "=m"(state->tr));

	state->dr0 = co_get_dr0();
	state->dr1 = co_get_dr1();
	state->dr2 = co_get_dr2();
	state->dr3 = co_get_dr3();
	state->dr6 = co_get_dr6();
	state->dr7 = co_get_dr7();
}
