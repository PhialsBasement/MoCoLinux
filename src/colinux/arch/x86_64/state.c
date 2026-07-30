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

/*
 * Put the state back.
 *
 * This is the restore half, and the ordering below is the whole substance of it.
 * Three things are easy to get wrong and each is fatal:
 *
 *  1. Segment selectors must be reloaded BEFORE the segment-base MSRs. Writing
 *     %fs or %gs in long mode loads the base from the descriptor -- zero, for a
 *     flat one -- and so destroys whatever FS_BASE/GS_BASE held. Restoring the
 *     MSRs first and the selectors second would silently undo the MSR writes and
 *     leave Windows with a null KPCR pointer.
 *
 *  2. ltr faults with #GP unless the TSS descriptor is marked available rather
 *     than busy, and the CPU sets it busy on load -- so reloading the same TR
 *     requires clearing that bit first. This is the one host structure that has
 *     to be written, and the write goes into the live GDT.
 *
 *  3. CS cannot be written with a mov. It is deliberately not restored here:
 *     this function puts back a state captured moments earlier in the same code
 *     segment, so CS is already correct. The real switch reloads it with a far
 *     return, which is a separate problem.
 *
 * Interrupts are off across the whole sequence, because the state is inconsistent
 * in the middle of it.
 */

/* Long-mode TSS descriptors are 16 bytes; busy is bit 9 of the second dword. */
#define CO_X86_TSS_BUSY_BIT 0x00000200

static void co_arch_clear_tss_busy(co_arch_state_stack_t* state)
{
	unsigned int* descriptor;

	if (state->tr == 0)
		return;

	/*
	 * The selector's index scales by 8 even though the descriptor occupies two
	 * such slots, so masking the low three bits gives the byte offset directly.
	 */
	descriptor = (unsigned int*)((unsigned char*)state->gdt.base + (state->tr & ~7));

	descriptor[1] &= ~CO_X86_TSS_BUSY_BIT;
}

void co_arch_restore_state(co_arch_state_stack_t* state)
{
	unsigned long long flags;
	unsigned short sel;

	asm volatile("pushfq; popq %0; cli" : "=r"(flags) : : "memory");

	/* Control registers. CR3 last of the three, so paging structures are
	 * consistent with CR4's paging-mode bits when it takes effect. */
	asm volatile("mov %0, %%cr0" : : "r"(state->cr0) : "memory");
	asm volatile("mov %0, %%cr4" : : "r"(state->cr4) : "memory");
	asm volatile("mov %0, %%cr2" : : "r"(state->cr2));
	asm volatile("mov %0, %%cr3" : : "r"(state->cr3) : "memory");

	/* Descriptor tables. */
	asm volatile("lgdt %0" : : "m"(state->gdt));
	asm volatile("lidt %0" : : "m"(state->idt));

	sel = (unsigned short)state->ldt;
	asm volatile("lldt %0" : : "m"(sel));

	co_arch_clear_tss_busy(state);
	sel = state->tr;
	asm volatile("ltr %0" : : "m"(sel));

	/* Selectors before the base MSRs -- see (1) above. */
	sel = (unsigned short)state->ds; asm volatile("mov %0, %%ds" : : "r"(sel));
	sel = (unsigned short)state->es; asm volatile("mov %0, %%es" : : "r"(sel));
	sel = (unsigned short)state->fs; asm volatile("mov %0, %%fs" : : "r"(sel));
	sel = (unsigned short)state->gs; asm volatile("mov %0, %%gs" : : "r"(sel));
	sel = (unsigned short)state->ss; asm volatile("mov %0, %%ss" : : "r"(sel));

	/* Now the bases the selector loads just cleared. */
	co_write_msr(CO_MSR_IA32_FS_BASE,        state->fs_base);
	co_write_msr(CO_MSR_IA32_GS_BASE,        state->gs_base);
	co_write_msr(CO_MSR_IA32_KERNEL_GS_BASE, state->kernel_gs_base);

	/*
	 * EFER with LMA masked out. LMA is read-only and maintained by hardware to
	 * say whether long mode is active; writing it back is at best ignored and
	 * on some parts raises #GP. LME, SCE and NXE are ours to set.
	 */
	co_write_msr(CO_MSR_IA32_EFER,   state->efer & ~(unsigned long long)CO_ARCH_X86_EFER_LMA);
	co_write_msr(CO_MSR_IA32_STAR,   state->star);
	co_write_msr(CO_MSR_IA32_LSTAR,  state->lstar);
	co_write_msr(CO_MSR_IA32_CSTAR,  state->cstar);
	co_write_msr(CO_MSR_IA32_SFMASK, state->sfmask);

	/* DR7 last: it is what actually arms the others. */
	asm volatile("mov %0, %%dr0" : : "r"(state->dr0));
	asm volatile("mov %0, %%dr1" : : "r"(state->dr1));
	asm volatile("mov %0, %%dr2" : : "r"(state->dr2));
	asm volatile("mov %0, %%dr3" : : "r"(state->dr3));
	asm volatile("mov %0, %%dr7" : : "r"(state->dr7));

	asm volatile("pushq %0; popfq" : : "r"(flags) : "memory", "cc");
}

/*
 * Save the state and put it straight back, without ever changing address space.
 * If this returns, the restore sequence and its ordering are right -- which is
 * the bulk of the risk in the world switch, isolated from the switch itself.
 */
void co_arch_test_save_restore(co_arch_state_stack_t* state)
{
	co_arch_save_state(state);
	co_arch_restore_state(state);
}
