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

/*
 * Offsets inside the passage page's first page, as literals because the fault
 * handler is assembly that has to find them without any register it can trust.
 * Asserted against the real structure below, so they cannot drift.
 */
#define CO_PP_HOST_STATE	"0x640"
#define CO_PP_LINUXVM_STATE	"0x780"
#define CO_PP_FAULTED		"0x8e8"
#define CO_PP_FAULT_RIP		"0x8f0"

#define CO_PP_HOST_STATE_N	0x640
#define CO_PP_LINUXVM_STATE_N	0x780
#define CO_PP_FAULTED_N		0x8e8
#define CO_PP_FAULT_RIP_N	0x8f0

/* params[6] and params[7]; params[8..9] are the guest GDT, so these are free. */
#define CO_PP_COUNTER		"0x8f8"
#define CO_PP_REGSUM		"0x900"
#define CO_PP_COUNTER_N		0x8f8
#define CO_PP_REGSUM_N		0x900

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
    /*
     * And the IDT with it, for exactly the same reason one step removed. IDTR is
     * not read by the switch itself -- interrupts are off and nothing faults, which
     * is why a round trip passes without this -- but the instant anything does
     * fault in the other address space, the CPU reads a gate from wherever IDTR
     * points. Left pointing at the host's table, that read is unmapped, and so is
     * the page fault handler that would report it: double fault, triple fault,
     * reset. The IDT travels with the address space, just like the GDT.
     */
    "    sidt " CO_ARCH_STATE_STACK_IDT "(%rcx)                     \n"
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
    "    lidt " CO_ARCH_STATE_STACK_IDT "(%rdx)                     \n"
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
    /*
     * Where a guest exception lands.
     *
     * Reached through the guest's own IDT, so it runs in the guest address space
     * and must live in the passage page. Nothing in any register can be trusted
     * -- the faulting code owned them -- so it recovers the passage page's base
     * from its own RIP, which works because this code is in the first page and
     * the page is 4 KB aligned.
     *
     * Then it records the fault and re-enters the switch with the guest as the
     * side being left. The host resumes exactly where it would have on a normal
     * return, and inspects the flag.
     *
     * This is interrupt forwarding in miniature, and the same shape the real one
     * takes: the guest catches, records, and hands control back rather than
     * trying to handle anything itself.
     */
    /*
     * An alternate guest entry that stores the sentinel and then deliberately
     * raises #UD. If the guest IDT works, this comes back through the handler
     * with the fault recorded; if it does not, it is a triple fault and a reset.
     * That is the whole difference being tested.
     */
    ".globl co_switch_guest_entry_fault                             \n"
    "co_switch_guest_entry_fault:                                   \n"
    "    mov %r9, (%r8)                                             \n"
    "    ud2                                                        \n"
    ".globl co_switch_guest_fault                                   \n"
    "co_switch_guest_fault:                                         \n"
    "    lea 0(%rip), %rax                                          \n"
    "    and $-4096, %rax                                           \n"
    "    movq $1, " CO_PP_FAULTED "(%rax)                           \n"
    /* the interrupt frame's RIP, before anything is pushed over it */
    "    mov (%rsp), %rdx                                           \n"
    "    mov %rdx, " CO_PP_FAULT_RIP "(%rax)                        \n"
    "    lea " CO_PP_LINUXVM_STATE "(%rax), %rcx                    \n"
    "    lea " CO_PP_HOST_STATE "(%rax), %rdx                       \n"
    "    jmp co_switch_full                                         \n"
    /*
     * A guest that yields and expects to be resumed -- the shape the real
     * monitor loop has, reduced to something with no Linux in it.
     *
     * Each time round it bumps a counter in the passage page, then calls the
     * switch to hand control back to the host. Because it *calls* rather than
     * jumps, there is a return address on the guest stack, so when the host
     * re-enters, the switch's epilogue returns here and the loop continues from
     * where it stopped. That is the whole property being tested: the first entry
     * arrives at the top, every later one resumes inside co_switch_full.
     *
     * rbx is zeroed once, above the loop label, and incremented inside it. It is
     * callee-saved, so it lives on the guest stack across each yield -- if it
     * still counts correctly after N crossings, guest registers genuinely
     * survive being switched away from and back to.
     *
     * Nothing is assumed about register contents on resume: rax, rcx and rdx are
     * all recomputed from RIP each iteration, exactly as the fault handler does.
     */
    ".globl co_switch_guest_loop                                    \n"
    "co_switch_guest_loop:                                          \n"
    "    xor %rbx, %rbx               /* first entry only */        \n"
    "2:  lea 0(%rip), %rax                                          \n"
    "    and $-4096, %rax                                           \n"
    "    incq " CO_PP_COUNTER "(%rax)                               \n"
    "    inc %rbx                                                   \n"
    "    mov %rbx, " CO_PP_REGSUM "(%rax)                           \n"
    "    lea " CO_PP_LINUXVM_STATE "(%rax), %rcx                    \n"
    "    lea " CO_PP_HOST_STATE "(%rax), %rdx                       \n"
    "    call co_switch_full                                        \n"
    "    jmp 2b                                                     \n"
    ".globl co_switch_full_end                                      \n"
    "co_switch_full_end:                                            \n");

extern char co_switch_guest_fault;
extern char co_switch_guest_entry_fault;
extern char co_switch_guest_loop;

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
static bool_t co_build_guest_tables(co_arch_passage_page_t* pp, unsigned long long va)
{
	co_pa_t first_pa, pt_pa, pd_pa, pdpt_pa;
	unsigned long long last_va = va + sizeof(*pp) - 1;

	/*
	 * One PT is installed, at one PD slot, so all 15 pages have to fall inside the
	 * same 2 MB region. NonPagedPool has no reason to honour that, and when it does
	 * not, CO_ARCH_PTE_INDEX wraps and the tail pages are quietly mapped nowhere --
	 * which the guest only discovers by faulting on an unmapped stack or IDT. Refuse
	 * instead, so a bad allocation is a failed test and not a reset.
	 */
	if (CO_ARCH_PMD_INDEX(va)  != CO_ARCH_PMD_INDEX(last_va) ||
	    CO_ARCH_PUD_INDEX(va)  != CO_ARCH_PUD_INDEX(last_va) ||
	    CO_ARCH_PGD_INDEX(va)  != CO_ARCH_PGD_INDEX(last_va)) {
		co_debug_error("passage page 0x%llx..0x%llx straddles a 2MB boundary",
			       va, last_va);
		return PFALSE;
	}

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

	return PTRUE;
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

	if (!co_build_guest_tables(pp, va)) {
		co_os_free_exec_pages(pp, pages);
		return CO_RC(ERROR);
	}
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

	out->observed   = *sentinel;
	out->faulted    = (int)pp->params[4];
	out->fault_rip  = pp->params[5];
	out->succeeded  = (out->observed == CO_SWITCH_SENTINEL) ? PTRUE : PFALSE;

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
/* Long-mode interrupt gate: 16 bytes, the offset split across three fields. */
struct co_x86_64_gate {
	unsigned short offset_low;
	unsigned short selector;
	unsigned short flags;
	unsigned short offset_mid;
	unsigned int   offset_high;
	unsigned int   reserved;
} __attribute__((packed));

typedef char co_assert_gate_is_16[(sizeof(struct co_x86_64_gate) == 16) ? 1 : -1];

/*
 * The handler writes its record at literal offsets because it has no register it
 * can trust; the C side reads it back through params[]. Tie the two together so
 * the report cannot silently start reading the wrong words.
 */
typedef char co_assert_fault_slots
	[(__builtin_offsetof(co_arch_passage_page_t, params) + 4 * 8 == CO_PP_FAULTED_N &&
	  __builtin_offsetof(co_arch_passage_page_t, params) + 5 * 8 == CO_PP_FAULT_RIP_N &&
	  __builtin_offsetof(co_arch_passage_page_t, params) + 6 * 8 == CO_PP_COUNTER_N &&
	  __builtin_offsetof(co_arch_passage_page_t, params) + 7 * 8 == CO_PP_REGSUM_N)
	 ? 1 : -1];

/* The IDT lives in host_temp; a full 256-gate table has to fit inside it. */
typedef char co_assert_idt_fits_host_temp
	[(sizeof(((co_arch_passage_page_t*)0)->host_temp) >= 256 * 16) ? 1 : -1];

/*
 * Point every vector at the same handler. Vector identity is not recovered --
 * that needs 256 stubs, one per vector, to push their own number -- but the
 * faulting RIP is, which is enough to know where it went wrong.
 *
 * Note the gates whose vectors push an error code (#PF, #GP, #DF among them)
 * leave the interrupt frame shifted by eight bytes, so the RIP this records is
 * only correct for the vectors that do not. #UD, which the test raises, does not.
 */
static void co_build_guest_idt(struct co_x86_64_gate* idt,
			       unsigned long long handler,
			       unsigned short selector)
{
	int vector;

	for (vector = 0; vector < 256; vector++) {
		idt[vector].offset_low  = (unsigned short)(handler & 0xffff);
		idt[vector].selector    = selector;
		idt[vector].flags       = 0x8e00;	/* present, DPL 0, interrupt gate */
		idt[vector].offset_mid  = (unsigned short)((handler >> 16) & 0xffff);
		idt[vector].offset_high = (unsigned int)(handler >> 32);
		idt[vector].reserved    = 0;
	}
}

/*
 * Build a passage page carrying a guest address space, a GDT and IDT of its own,
 * and a state block derived from the host's, with the guest set to start at
 * entry_offset into the blob.
 *
 * Everything the round-trip and resume tests have in common. They differ only in
 * where the guest starts and what is done with it afterwards, and keeping the
 * setup in one place is what stops those two drifting apart.
 *
 * Returns the page with *out partly filled in, or NULL.
 */
static co_arch_passage_page_t* co_setup_guest_page(co_arch_switch_test_t* out,
						   unsigned long entry_offset)
{
	co_arch_passage_page_t* pp;
	unsigned long long va, guest_cr3;
	unsigned long blob_size;
	int pages = sizeof(co_arch_passage_page_t) / CO_ARCH_PAGE_SIZE;

	/* The handler reaches these by literal offset; keep them honest. */
	if (__builtin_offsetof(co_arch_passage_page_t, host_state)    != CO_PP_HOST_STATE_N ||
	    __builtin_offsetof(co_arch_passage_page_t, linuxvm_state) != CO_PP_LINUXVM_STATE_N) {
		co_debug_error("passage page layout moved; fault handler offsets are stale");
		return NULL;
	}

	blob_size      = (unsigned long)(&co_switch_full_end - &co_switch_full);
	out->code_size = blob_size;

	if (blob_size > sizeof(pp->code))
		return NULL;

	pp = co_os_alloc_exec_pages(pages);
	if (pp == NULL)
		return NULL;

	co_memset(pp, 0, sizeof(*pp));

	va = (unsigned long long)(size_t)pp;
	if (!co_build_guest_tables(pp, va)) {
		co_os_free_exec_pages(pp, pages);
		return NULL;
	}
	guest_cr3 = co_os_virt_to_phys(&pp->guest_temp.pml4);

	co_memcpy(pp->code, &co_switch_full, blob_size);

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

	/*
	 * An IDT of its own, also inside the passage page. Without one, IDTR still
	 * points at the host's table after the crossing, and any exception in the
	 * guest is an unreachable handler: double fault, triple fault, reset. With
	 * one, a fault becomes a clean return with a flag set.
	 *
	 * 256 gates of 16 bytes is exactly 4 KB and first_page is full, so it needs a
	 * page of its own. It goes in host_temp, which this path never builds -- and
	 * emphatically NOT at pp + CO_ARCH_PAGE_SIZE, which is guest_temp.pml4: writing
	 * the IDT there overwrites the guest's top-level page table after it has been
	 * built, and the CR3 load then triple faults on the first instruction fetch.
	 * Naming the member rather than doing pointer arithmetic is what keeps that
	 * honest, so leave it named.
	 */
	{
		struct co_x86_64_gate* idt = (struct co_x86_64_gate*)&pp->host_temp;
		unsigned long long handler = (unsigned long long)(size_t)pp->code
			+ (unsigned long)(&co_switch_guest_fault - &co_switch_full);

		co_build_guest_idt(idt, handler, 0x08);

		pp->linuxvm_state.idt.table = (struct x86_idt_entry*)idt;
		pp->linuxvm_state.idt.size  = (256 * 16) - 1;

		out->guest_idt     = (unsigned long long)(size_t)idt;
		out->fault_handler = handler;
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

	return pp;
}

co_rc_t co_arch_test_roundtrip(co_manager_t* manager, co_arch_switch_test_t* out,
			       bool_t provoke_fault)
{
	co_arch_passage_page_t* pp;
	co_switch_full_fn fn;
	unsigned long long* sentinel;
	unsigned long entry_offset;
	int pages = sizeof(co_arch_passage_page_t) / CO_ARCH_PAGE_SIZE;

	co_memset(out, 0, sizeof(*out));
	out->supported = PTRUE;

	entry_offset = provoke_fault
		? (unsigned long)(&co_switch_guest_entry_fault - &co_switch_full)
		: (unsigned long)(&co_switch_guest_entry - &co_switch_full);

	pp = co_setup_guest_page(out, entry_offset);
	if (pp == NULL)
		return CO_RC(ERROR);

	fn       = (co_switch_full_fn)(void*)pp->code;
	sentinel = (unsigned long long*)&pp->params[0];
	out->expected = CO_SWITCH_SENTINEL;

	fn(&pp->host_state, &pp->linuxvm_state, sentinel, CO_SWITCH_SENTINEL);

	out->observed  = *sentinel;
	/*
	 * Where the fault handler left its record. It cannot be handed a pointer -- it
	 * runs with no trustworthy register -- so it writes at fixed offsets into the
	 * passage page, and params[4] and params[5] are those offsets (0x8e8, 0x8f0).
	 */
	out->faulted   = (int)pp->params[4];
	out->fault_rip = pp->params[5];
	out->succeeded = (out->observed == CO_SWITCH_SENTINEL) ? PTRUE : PFALSE;

	co_os_free_exec_pages(pp, pages);

	return CO_RC(OK);
}

/*
 * Enter the guest repeatedly and require it to pick up where it stopped.
 *
 * This is the monitor loop with the Linux taken out. The first entry lands at the
 * top of the guest loop; every later one resumes *inside* co_switch_full, because
 * the guest's own outbound switch overwrote linuxvm_state.return_rip and .rsp with
 * its resume point. So nothing between iterations may reinitialise those two
 * fields -- doing so would silently turn this back into N independent first
 * entries and the test would pass while proving nothing.
 *
 * Two things are checked, and they fail differently:
 *   counter   -- memory in the passage page. Wrong => the guest did not resume.
 *   reg_accum -- kept in rbx, which is callee-saved and therefore lives on the
 *                guest stack across each crossing. Wrong while counter is right
 *                => the guest resumed but its registers did not survive.
 */
co_rc_t co_arch_test_resume(co_manager_t* manager, co_arch_switch_test_t* out,
			    int iterations)
{
	co_arch_passage_page_t* pp;
	co_switch_full_fn fn;
	unsigned long long* sentinel;
	int pages = sizeof(co_arch_passage_page_t) / CO_ARCH_PAGE_SIZE;
	int i;

	co_memset(out, 0, sizeof(*out));
	out->supported = PTRUE;

	if (iterations < 1 || iterations > 4096)
		iterations = 64;

	pp = co_setup_guest_page(out,
		(unsigned long)(&co_switch_guest_loop - &co_switch_full));
	if (pp == NULL)
		return CO_RC(ERROR);

	fn       = (co_switch_full_fn)(void*)pp->code;
	sentinel = (unsigned long long*)&pp->params[0];

	out->iterations = iterations;
	out->expected   = (unsigned long long)iterations;

	for (i = 0; i < iterations; i++)
		fn(&pp->host_state, &pp->linuxvm_state, sentinel, CO_SWITCH_SENTINEL);

	out->counter   = pp->params[6];
	out->reg_accum = pp->params[7];
	out->observed  = out->counter;
	out->faulted   = (int)pp->params[4];
	out->fault_rip = pp->params[5];

	out->succeeded = (out->counter   == (unsigned long long)iterations &&
			  out->reg_accum == (unsigned long long)iterations &&
			  !out->faulted) ? PTRUE : PFALSE;

	co_os_free_exec_pages(pp, pages);

	return CO_RC(OK);
}
