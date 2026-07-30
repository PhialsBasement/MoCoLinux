/*
 * This source code is a part of coLinux source package.
 *
 * The code is licensed under the GPL. See the COPYING file at
 * the root directory.
 */

/*
 * The first half of a world switch: change CR3 and keep executing.
 *
 * Everything the passage code does rests on one property -- that the page the
 * code lives in is mapped at the same virtual address in both address spaces, so
 * instruction fetch continues across the CR3 write. This tests exactly that and
 * nothing else. No state is saved or restored, no control is transferred, and
 * the guest is not a kernel; it is a store instruction.
 *
 * The window between the two CR3 writes is unforgiving. The guest address space
 * maps precisely one page, so anything else touched in that window -- the stack,
 * the driver's own code, an interrupt vectoring through the host's IDT to a
 * handler that is no longer mapped -- is a page fault whose handler is also
 * unmapped, which is a double fault, which is a triple fault, which is a reset.
 * So the window contains no stack access, no call, no memory reference outside
 * the passage page, and runs with interrupts off.
 *
 * CR4.PGE is cleared first, and that is not incidental. PGE is set on this host
 * and Windows marks its kernel pages global, so global TLB entries survive a CR3
 * write. Without flushing them the host's mappings would remain live inside the
 * guest address space and the test would pass whether or not the guest page
 * tables were correct -- which is worse than no test. This is the same reason the
 * i386 passage code clears PGE on its way through.
 */

/* GCC's own freestanding stddef.h, for size_t as a pointer-sized integer. */
#include <stddef.h>

#include <colinux/common/common.h>
#include <colinux/common/debug.h>
#include <colinux/common/libc.h>
#include <colinux/kernel/manager.h>
#include <colinux/os/kernel/alloc.h>
#include <colinux/os/kernel/misc.h>
#include <colinux/arch/switch.h>
#include <colinux/arch/state.h>

#include "mmu.h"
#include "utils.h"
#include "defs.h"

/*
 * Position independent by construction: no RIP-relative operand, no absolute
 * address, no symbol reference. It is copied into the passage page and called
 * there, so it must not care where it lives.
 *
 * Win64 argument registers: rcx, rdx, r8, r9.
 *   rcx = host CR3      rdx = guest CR3
 *   r8  = sentinel address, inside the passage page
 *   r9  = value to store
 */
extern char co_switch_probe_code;
extern char co_switch_probe_code_end;

asm(".text                                          \n"
    ".globl co_switch_probe_code                    \n"
    "co_switch_probe_code:                          \n"
    "    pushfq                                     \n"
    "    cli                                        \n"
    /* flush global TLB entries, or the host stays mapped and the test lies */
    "    mov %cr4, %rax                             \n"
    "    mov %rax, %r10                             \n"
    "    btr $7, %rax                               \n"   /* CR4.PGE */
    "    mov %rax, %cr4                             \n"
    /* --- into the guest address space --- */
    "    mov %rdx, %cr3                             \n"
    /* the only memory reference permitted here, and it is in the mapped page */
    "    mov %r9, (%r8)                             \n"
    /* --- back to the host --- */
    "    mov %rcx, %cr3                             \n"
    "    mov %r10, %rax                             \n"
    "    mov %rax, %cr4                             \n"
    "    popfq                                      \n"
    "    ret                                        \n"
    ".globl co_switch_probe_code_end                \n"
    "co_switch_probe_code_end:                      \n");


/*
 * A bidirectional switch: transfer control into the guest address space and back.
 *
 * One blob with two entry points, both position independent, both living in the
 * passage page so they stay mapped across the CR3 write.
 *
 *   co_switch_full        save where we are, enter the other address space,
 *                         far-return to the other side's CS:RIP
 *   co_switch_guest_entry where the guest lands: store the sentinel, then run the
 *                         same switch with the two state pointers exchanged,
 *                         which brings the host back
 *
 * Scope is deliberately narrower than a real world switch: only CR3, RSP and
 * CS:RIP are exchanged. Host and guest here share one machine's descriptor
 * tables and MSRs, so restoring them would be writing back identical values --
 * that half is proven separately by co_arch_test_save_restore(), and folding it
 * in would make a failure ambiguous between the two.
 *
 * rcx and rdx carry the two state pointers, and r8/r9 the sentinel address and
 * value. None are touched by the switch, so they survive the far return and are
 * still valid when the guest starts -- which is how the guest knows anything.
 */
extern char co_switch_full;
extern char co_switch_guest_entry;
extern char co_switch_full_end;

asm(".text                                                          \n"
    ".globl co_switch_full                                          \n"
    "co_switch_full:                                                \n"
    /* rcx = state we are leaving, rdx = state we are entering */
    "    push %rbx                                                  \n"
    "    push %rbp                                                  \n"
    "    push %rdi                                                  \n"
    "    push %rsi                                                  \n"
    "    push %r12                                                  \n"
    "    push %r13                                                  \n"
    "    push %r14                                                  \n"
    "    push %r15                                                  \n"
    "    pushfq                                                     \n"
    "    cli                                                        \n"
    /* record where to resume, and on what stack, in the state we are leaving */
    "    lea 1f(%rip), %rax                                         \n"
    "    mov %rax, " CO_ARCH_STATE_STACK_RETURN_RIP "(%rcx)         \n"
    "    mov %rsp, " CO_ARCH_STATE_STACK_RSP "(%rcx)                \n"
    "    mov %cs, %eax                                              \n"
    "    mov %rax, " CO_ARCH_STATE_STACK_CS "(%rcx)                 \n"
    /*
     * The GDT has to travel with the address space. lretq does not merely set
     * CS:RIP, it loads a segment descriptor -- so it reads the GDT. Leaving GDTR
     * pointing at the host's table across the CR3 write means that read lands on
     * an unmapped page, and the page-fault handler is unmapped too: double fault,
     * triple fault, instant reset with no bugcheck. Learned the hard way.
     */
    "    sgdt " CO_ARCH_STATE_STACK_GDT "(%rcx)                     \n"
    /* drop global TLB entries, or the old address space stays visible */
    "    mov %cr4, %rax                                             \n"
    "    mov %rax, %r10                                             \n"
    "    btr $7, %rax                                               \n"
    "    mov %rax, %cr4                                             \n"
    /* --- the crossing --- */
    "    mov " CO_ARCH_STATE_STACK_CR3 "(%rdx), %rax                \n"
    "    mov %rax, %cr3                                             \n"
    "    mov %r10, %rax                                             \n"
    "    mov %rax, %cr4                                             \n"
    /*
     * Now in the other address space, so its GDT is reachable. This must come
     * after the CR3 write and before the lretq that reads it.
     */
    "    lgdt " CO_ARCH_STATE_STACK_GDT "(%rdx)                     \n"
    /* onto the other side's stack, then far-return to its cs:rip */
    "    mov " CO_ARCH_STATE_STACK_RSP "(%rdx), %rsp                \n"
    "    push " CO_ARCH_STATE_STACK_CS "(%rdx)                      \n"
    "    push " CO_ARCH_STATE_STACK_RETURN_RIP "(%rdx)              \n"
    "    lretq                                                      \n"
    /* the other side switching back lands here, on our own stack again */
    "1:  popfq                                                      \n"
    "    pop %r15                                                   \n"
    "    pop %r14                                                   \n"
    "    pop %r13                                                   \n"
    "    pop %r12                                                   \n"
    "    pop %rsi                                                   \n"
    "    pop %rdi                                                   \n"
    "    pop %rbp                                                   \n"
    "    pop %rbx                                                   \n"
    "    ret                                                        \n"
    ".globl co_switch_guest_entry                                   \n"
    "co_switch_guest_entry:                                         \n"
    /* running in the guest address space now, on the guest stack */
    "    mov %r9, (%r8)                                             \n"
    /* swap the roles and go back the way we came */
    "    xchg %rcx, %rdx                                            \n"
    "    jmp co_switch_full                                         \n"
    ".globl co_switch_full_end                                      \n"
    "co_switch_full_end:                                            \n");

typedef void (*co_switch_full_fn)(co_arch_state_stack_t* leaving,
                                  co_arch_state_stack_t* entering,
                                  unsigned long long* sentinel,
                                  unsigned long long value);

typedef void (*co_switch_probe_fn)(unsigned long long host_cr3,
				   unsigned long long guest_cr3,
				   unsigned long long* sentinel,
				   unsigned long long value);

#define CO_SWITCH_SENTINEL 0x5741544348454421ULL	/* "SWATCHE!" */

/*
 * A guest address space containing one mapping: the passage page, at the address
 * the host already has it. Same address on both sides is what makes the switch
 * simple -- there is no other_map delta and nothing has to relocate itself.
 */
static void co_build_guest_tables(co_arch_passage_page_t* pp, unsigned long long va)
{
	co_pa_t first_pa, pt_pa, pd_pa, pdpt_pa;

	co_memset(&pp->guest_temp, 0, sizeof(pp->guest_temp));

	first_pa = co_os_virt_to_phys(&pp->first_page);
	pt_pa    = co_os_virt_to_phys(&pp->guest_temp.pt[0]);
	pd_pa    = co_os_virt_to_phys(&pp->guest_temp.pd[0]);
	pdpt_pa  = co_os_virt_to_phys(&pp->guest_temp.pdpt[0]);

	/*
	 * Map every page of the passage page, not just the first. The guest needs a
	 * stack to be far-returned onto and to push from, and the only memory it can
	 * touch is what this table maps. NonPagedPool is not physically contiguous,
	 * so each page is looked up individually.
	 */
	{
		int page;

		for (page = 0; page < (int)(sizeof(*pp) / CO_ARCH_PAGE_SIZE); page++) {
			unsigned char* p = (unsigned char*)pp + page * CO_ARCH_PAGE_SIZE;
			unsigned long long page_va = va + page * CO_ARCH_PAGE_SIZE;

			pp->guest_temp.pt[0][CO_ARCH_PTE_INDEX(page_va)] =
				co_os_virt_to_phys(p) | _KERNPG_TABLE;
		}
	}

	pp->guest_temp.pt[0][CO_ARCH_PTE_INDEX(va)]   = first_pa | _KERNPG_TABLE;
	pp->guest_temp.pd[0][CO_ARCH_PMD_INDEX(va)]   = pt_pa    | _KERNPG_TABLE;
	pp->guest_temp.pdpt[0][CO_ARCH_PUD_INDEX(va)] = pd_pa    | _KERNPG_TABLE;
	pp->guest_temp.pml4[CO_ARCH_PGD_INDEX(va)]    = pdpt_pa  | _KERNPG_TABLE;
}

co_rc_t co_arch_test_switch(co_manager_t* manager, co_arch_switch_test_t* out)
{
	co_arch_passage_page_t* pp;
	co_switch_probe_fn fn;
	unsigned long long va, guest_cr3, host_cr3;
	unsigned long long* sentinel;
	unsigned long code_size;
	int pages = sizeof(co_arch_passage_page_t) / CO_ARCH_PAGE_SIZE;

	co_memset(out, 0, sizeof(*out));
	out->supported = PTRUE;

	code_size = (unsigned long)(&co_switch_probe_code_end - &co_switch_probe_code);
	out->code_size = code_size;
	if (code_size > sizeof(pp->code)) {
		co_debug_error("switch probe is %lu bytes, code area is %lu",
			       code_size, (unsigned long)sizeof(pp->code));
		return CO_RC(ERROR);
	}

	pp = co_os_alloc_exec_pages(pages);
	if (pp == NULL)
		return CO_RC(OUT_OF_MEMORY);

	co_memset(pp, 0, sizeof(*pp));

	va       = (unsigned long long)(size_t)pp;
	host_cr3 = co_get_cr3();

	co_build_guest_tables(pp, va);
	guest_cr3 = co_os_virt_to_phys(&pp->guest_temp.pml4);

	co_memcpy(pp->code, &co_switch_probe_code, code_size);

	/* Both live in first_page, the only page the guest address space maps. */
	fn       = (co_switch_probe_fn)(void*)pp->code;
	sentinel = (unsigned long long*)&pp->params[0];

	out->passage_va = va;
	out->passage_pa = co_os_virt_to_phys(&pp->first_page);
	out->host_cr3   = host_cr3;
	out->guest_cr3  = guest_cr3;
	out->code_va    = (unsigned long long)(size_t)fn;
	out->expected   = CO_SWITCH_SENTINEL;

	fn(host_cr3, guest_cr3, sentinel, CO_SWITCH_SENTINEL);

	out->observed  = *sentinel;
	out->succeeded = (out->observed == CO_SWITCH_SENTINEL) ? PTRUE : PFALSE;

	co_os_free_exec_pages(pp, pages);

	return CO_RC(OK);
}

/*
 * The round trip: enter the guest address space, run code there, come back.
 *
 * The guest's state is built to look like the host's except for CR3, the stack
 * and the entry point. That is honest for what this tests -- both sides are the
 * same machine, so the descriptor tables and MSRs are genuinely identical, and
 * restoring them is proven separately.
 */
co_rc_t co_arch_test_roundtrip(co_manager_t* manager, co_arch_switch_test_t* out)
{
	co_arch_passage_page_t* pp;
	co_switch_full_fn fn;
	unsigned long long va, guest_cr3;
	unsigned long long* sentinel;
	unsigned long blob_size, entry_offset;
	int pages = sizeof(co_arch_passage_page_t) / CO_ARCH_PAGE_SIZE;

	co_memset(out, 0, sizeof(*out));
	out->supported = PTRUE;

	blob_size    = (unsigned long)(&co_switch_full_end - &co_switch_full);
	entry_offset = (unsigned long)(&co_switch_guest_entry - &co_switch_full);
	out->code_size = blob_size;

	if (blob_size > sizeof(pp->code))
		return CO_RC(ERROR);

	pp = co_os_alloc_exec_pages(pages);
	if (pp == NULL)
		return CO_RC(OUT_OF_MEMORY);

	co_memset(pp, 0, sizeof(*pp));

	va = (unsigned long long)(size_t)pp;
	co_build_guest_tables(pp, va);
	guest_cr3 = co_os_virt_to_phys(&pp->guest_temp.pml4);

	co_memcpy(pp->code, &co_switch_full, blob_size);

	fn       = (co_switch_full_fn)(void*)pp->code;
	sentinel = (unsigned long long*)&pp->params[0];

	/* Capture the host state, then derive the guest's from it. */
	co_arch_save_state(&pp->host_state);
	co_memcpy(&pp->linuxvm_state, &pp->host_state, sizeof(pp->linuxvm_state));

	/*
	 * A GDT of its own, living in the passage page so it is mapped in the guest
	 * address space. Two entries are enough for a far return: the mandatory null
	 * descriptor, and one 64-bit code segment.
	 *
	 *   0x00209a0000000000 -- P=1 DPL=0 S=1 type=code/read, L=1 (64-bit)
	 *
	 * This is the same thing the real design does: the guest kernel supplies its
	 * own gdt_table and the switch loads it.
	 */
	{
		unsigned long long* guest_gdt = &pp->params[8];

		guest_gdt[0] = 0x0000000000000000ULL;	/* null */
		guest_gdt[1] = 0x00209a0000000000ULL;	/* 64-bit code, selector 0x08 */

		pp->linuxvm_state.gdt.base  = (struct x86_dt_entry*)guest_gdt;
		pp->linuxvm_state.gdt.limit = (2 * 8) - 1;
		pp->linuxvm_state.cs        = 0x08;

		out->guest_gdt = (unsigned long long)(size_t)guest_gdt;
	}

	pp->linuxvm_state.cr3 = guest_cr3;
	/* entry point and stack both inside the passage page, which the guest maps */
	pp->linuxvm_state.return_rip = (unsigned long long)(size_t)pp->code + entry_offset;
	pp->linuxvm_state.rsp        = va + CO_ARCH_PAGE_SIZE - 0x40;

	out->passage_va = va;
	out->passage_pa = co_os_virt_to_phys(&pp->first_page);
	out->host_cr3   = pp->host_state.cr3;
	out->guest_cr3  = guest_cr3;
	out->code_va    = pp->linuxvm_state.return_rip;
	out->expected   = CO_SWITCH_SENTINEL;

	fn(&pp->host_state, &pp->linuxvm_state, sentinel, CO_SWITCH_SENTINEL);

	out->observed  = *sentinel;
	out->succeeded = (out->observed == CO_SWITCH_SENTINEL) ? PTRUE : PFALSE;

	co_os_free_exec_pages(pp, pages);

	return CO_RC(OK);
}
