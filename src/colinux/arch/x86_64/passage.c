/*
 * This source code is a part of coLinux source package.
 *
 * The code is licensed under the GPL. See the COPYING file at
 * the root directory.
 */

/*
 * The passage page and code are responsible for switching between the two
 * operating systems.
 *
 * The switch itself lives in switch.c, not here. This file carries the passage
 * page's allocation and teardown so that the driver builds and loads, and
 * co_host_switch_wrapper() refuses to switch rather than attempting it. That is
 * milestone M2 in doc/porting-x86_64: prove that an unsigned 64-bit driver loads
 * on XP x64 and answers ioctls, before writing a line of world-switch assembly.
 *
 * Writing that assembly is M3, and section 4 of doc/porting-x86_64 is the list
 * of what it has to do differently from the i386 version. The short form:
 *
 *   - Both sides are long mode, so there is no CR4.PAE to toggle and no reason
 *     to clear CR0.PG -- which is illegal in long mode anyway. A plain CR3 load
 *     between two 4-level trees suffices, and if the passage page can be mapped
 *     at the same virtual address in both address spaces then other_map is zero
 *     and the whole EIP-relocation dance disappears.
 *   - EFER, the SYSCALL MSRs and FS_BASE/GS_BASE/KERNEL_GS_BASE all have to be
 *     saved and restored. The segment-base MSRs are not optional: writing a
 *     selector to %fs or %gs does not restore the 64-bit base, and Windows keeps
 *     the KPCR at GS base.
 *   - Arguments arrive in registers, and in different registers depending on
 *     direction: the host driver is Win64, the guest kernel is SysV.
 *   - ljmp with immediate operands does not exist in 64-bit mode, so the
 *     self-modifying far jump that reloads CS has to become a far return.
 */

/* GCC's own freestanding stddef.h, for size_t as a pointer-sized integer. */
#include <stddef.h>

#include <colinux/common/debug.h>
#include <colinux/common/libc.h>
#include <colinux/common/common.h>
#include <colinux/kernel/monitor.h>
#include <colinux/arch/passage.h>
#include <colinux/os/kernel/alloc.h>
#include <colinux/os/kernel/misc.h>

#include "cpuid.h"
#include "manager.h"
#include "utils.h"
#include "defs.h"

#ifdef CO_COLINUX_KERNEL
#error "CO_COLINUX_KERNEL should never defined here"
#endif

#ifdef CONFIG_COOPERATIVE
#error "CONFIG_COOPERATIVE should never defined here"
#endif

co_rc_t co_monitor_arch_passage_page_alloc(co_monitor_t *cmon)
{
	co_rc_t rc;

	cmon->archdep = co_os_malloc(sizeof(*cmon->archdep));
	if (cmon->archdep == NULL)
		return CO_RC(OUT_OF_MEMORY);

	co_memset(cmon->archdep, 0, sizeof(*cmon->archdep));

	/*
	 * co_os_alloc_exec_pages(), not co_os_alloc_pages(): the passage code
	 * executes from this page. Measured on XP x64, the default allocator
	 * returns pages with NX set and PWT|PCD set -- neither executable nor
	 * cached. This is what removes any need for an antinx equivalent here.
	 */
	cmon->passage_page = co_os_alloc_exec_pages(sizeof(co_arch_passage_page_t)/CO_ARCH_PAGE_SIZE);
	if (cmon->passage_page == NULL) {
		rc = CO_RC(OUT_OF_MEMORY);
		goto error;
	}

	co_memset(cmon->passage_page, 0, sizeof(co_arch_passage_page_t));

	/*
	 * No co_arch_anti_nx_init() counterpart. The i386 port walks the host's live
	 * page tables to clear NX on this page; on x64 that walk is four levels deep
	 * and PatchGuard objects to page-table tampering, so the page has to be
	 * allocated executable instead. Nothing executes from it while the switch is
	 * unimplemented, so this is a loose end for M3, not a bug now.
	 */

	return CO_RC(OK);

error:
	co_monitor_arch_passage_page_free(cmon);
	return rc;
}

void co_monitor_arch_passage_page_free(co_monitor_t *cmon)
{
	if (cmon->archdep) {
		co_os_free(cmon->archdep);
		cmon->archdep = NULL;
	}
	if (cmon->passage_page) {
		co_os_free_exec_pages(cmon->passage_page, sizeof(co_arch_passage_page_t)/CO_ARCH_PAGE_SIZE);
		cmon->passage_page = NULL;
	}
}

static void co_passage_page_dump_state(const char *which, co_arch_state_stack_t *state)
{
	co_debug("%s cs:%04llx ds:%04llx es:%04llx fs:%04llx gs:%04llx ss:%04llx",
		 which, state->cs, state->ds, state->es, state->fs, state->gs, state->ss);

	co_debug("%s cr0:%016llx cr2:%016llx cr3:%016llx cr4:%016llx efer:%016llx",
		 which, state->cr0, state->cr2, state->cr3, state->cr4, state->efer);

	co_debug("%s gdt:%016llx:%04x idt:%016llx:%04x ldt:%04x tr:%04x",
		 which, (unsigned long long)(size_t)state->gdt.base, state->gdt.limit,
		 (unsigned long long)(size_t)state->idt.table, state->idt.size,
		 state->ldt, state->tr);

	co_debug("%s rip:%016llx flags:%016llx rsp:%016llx",
		 which, state->return_rip, state->flags, state->rsp);

	co_debug("%s fsbase:%016llx gsbase:%016llx kgsbase:%016llx",
		 which, state->fs_base, state->gs_base, state->kernel_gs_base);
}

co_rc_t co_monitor_arch_passage_page_init(co_monitor_t *cmon)
{
	co_arch_passage_page_t *pp = cmon->passage_page;

	/*
	 * Record what can be recorded without a switch implementation: the page's
	 * own physical address, and the host's debug registers, whose shadow copy
	 * lives in the first bytes of the page.
	 */
	pp->self_physical_address = co_os_virt_to_phys(&pp->first_page);

	pp->dr0 = co_get_dr0();
	pp->dr1 = co_get_dr1();
	pp->dr2 = co_get_dr2();
	pp->dr3 = co_get_dr3();
	pp->dr6 = co_get_dr6();
	pp->dr7 = co_get_dr7();

	co_debug("passage page at %p, physical %016llx, %lu pages",
		 pp, (unsigned long long)pp->self_physical_address,
		 (unsigned long)(sizeof(co_arch_passage_page_t)/CO_ARCH_PAGE_SIZE));

	co_passage_page_dump_state("host", &pp->host_state);
	co_passage_page_dump_state("linux", &pp->linuxvm_state);

	/*
	 * The switch itself exists and works -- arch/x86_64/switch.c, exercised by
	 * the --test-* and --boot-kernel paths, which build their own passage pages
	 * directly. What is missing is the wiring between it and *this* entry point,
	 * the one the monitor uses: a monitor instance, pseudo-physical memory, and
	 * a guest kernel that speaks the passage-page protocol.
	 *
	 * So this still does not build the guest entry state, and the wrapper below
	 * still refuses. Half-initialising the page would be worse than not
	 * initialising it.
	 */
	co_debug_error("x86-64 monitor path is not wired to the switch yet; "
		       "the driver can load, answer ioctls, and run the switch tests, "
		       "but co_monitor cannot run a guest");

	return CO_RC_OK;
}

void co_host_switch_wrapper(co_monitor_t *cmon)
{
	/*
	 * Refuse, loudly and every time. This is the one place that would otherwise
	 * hand control to a passage page containing nothing but zeroes -- the switch
	 * works, but nothing has initialised *this* page for it.
	 */
	co_debug_error("refusing to switch: the monitor's passage page is not built");

	co_passage_page->operation = CO_OPERATION_TERMINATE;
	co_passage_page->params[0] = CO_TERMINATE_INVALID_OPERATION;
}
