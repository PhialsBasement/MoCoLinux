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
