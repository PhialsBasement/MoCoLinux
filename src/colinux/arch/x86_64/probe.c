/*
 * This source code is a part of coLinux source package.
 *
 * The code is licensed under the GPL. See the COPYING file at
 * the root directory.
 */

/*
 * Walk the host's own page tables for one virtual address.
 *
 * This exists to answer a question the passage code's shape depends on: is the
 * address window the guest wants also unused by the host? If it is, the passage
 * page can be mapped at the same virtual address in both address spaces, which
 * makes other_map zero and means the switch never has to relocate its own
 * instruction pointer between two mappings -- the single largest simplification
 * available over the i386 code. See doc/porting-x86_64 section 4.1.
 *
 * Read-only, and deliberately so: it maps one page-table page at a time, reads a
 * single entry, and unmaps it. Nothing is written, and the host's tables are
 * never edited -- unlike the i386 antinx code, which clears NX in place and is
 * exactly the sort of thing PatchGuard exists to notice.
 */

#include <colinux/common/common.h>
#include <colinux/common/debug.h>
#include <colinux/kernel/manager.h>
#include <colinux/os/kernel/alloc.h>
#include <colinux/arch/probe.h>

#include "mmu.h"
#include "utils.h"

void co_arch_probe_va(co_manager_t *manager, co_manager_ioctl_probe_va_t *params)
{
	unsigned long long va = params->va;
	unsigned long long cr3;
	co_pfn_t pfn;
	int level;

	/* Index at each level, outermost first: PML4, PDPT, PD, PT. */
	const unsigned long index[CO_PROBE_VA_LEVELS] = {
		CO_ARCH_PGD_INDEX(va),
		CO_ARCH_PUD_INDEX(va),
		CO_ARCH_PMD_INDEX(va),
		CO_ARCH_PTE_INDEX(va),
	};

	params->supported     = PTRUE;
	params->present       = PFALSE;
	params->large_page    = PFALSE;
	params->levels_walked = 0;
	for (level = 0; level < CO_PROBE_VA_LEVELS; level++)
		params->entry[level] = 0;

	if (!CO_ARCH_VA_CANONICAL(va)) {
		co_debug_error("probe: 0x%llx is not a canonical address", va);
		params->rc = CO_RC(INVALID_PARAMETER);
		return;
	}

	cr3 = co_get_cr3();
	params->cr3 = cr3;

	/*
	 * CR3 holds the PML4's frame. Its low bits carry flags (and a PCID when
	 * CR4.PCIDE is set, which XP x64 never enables), so mask them off.
	 */
	pfn = (cr3 & ~0xfffULL) >> CO_ARCH_PAGE_SHIFT;

	for (level = 0; level < CO_PROBE_VA_LEVELS; level++) {
		unsigned long long *table;
		unsigned long long entry;

		table = co_os_map(manager, pfn);
		if (table == NULL) {
			co_debug_error("probe: could not map table pfn 0x%llx at level %d",
				       (unsigned long long)pfn, level);
			params->rc = CO_RC(ERROR);
			return;
		}

		entry = table[index[level]];
		co_os_unmap(manager, table, pfn);

		params->entry[level] = entry;
		params->levels_walked = level + 1;

		if (!(entry & _PAGE_PRESENT))
			break;		/* nothing mapped here; params->present stays false */

		/*
		 * PSE at the PDPT or PD level terminates the walk early: the entry
		 * maps a 1 GB or 2 MB page directly rather than pointing at another
		 * table. Either way the range is in use.
		 */
		if ((entry & _PAGE_PSE) && level < CO_PROBE_VA_LEVELS - 1) {
			params->large_page = PTRUE;
			params->present    = PTRUE;
			break;
		}

		if (level == CO_PROBE_VA_LEVELS - 1) {
			params->present = PTRUE;
			break;
		}

		pfn = (entry & ~0xfffULL & ~CO_ARCH_PAGE_NX) >> CO_ARCH_PAGE_SHIFT;
	}

	params->rc = CO_RC(OK);
}
