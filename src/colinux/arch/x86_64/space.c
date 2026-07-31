/*
 * This source code is a part of coLinux source package.
 *
 * The code is licensed under the GPL. See the COPYING file at
 * the root directory.
 */

/*
 * Four-level page tables for a guest address space.
 *
 * Every intermediate table is a host page obtained from co_os_get_page() and
 * touched only through co_os_map()/co_os_unmap(), so nothing here needs the
 * tables to be permanently mapped in the host's own address space -- which
 * matters, because on x86-64 there can be a great many of them and the host is
 * not obliged to give us a contiguous window.
 *
 * Teardown walks the tree rather than keeping a side list of what was allocated.
 * That is not merely tidier: a side list can disagree with the tree, and the way
 * it disagrees is by freeing a page that is still referenced. Walking cannot get
 * that wrong. The rule is that PML4/PDPT/PD/PT pages are ours and are freed,
 * while whatever a PT entry points at was supplied by the caller and is not.
 */

/* GCC's own freestanding stddef.h, for size_t as a pointer-sized integer. */
#include <stddef.h>

#include <colinux/common/common.h>
#include <colinux/common/debug.h>
#include <colinux/common/libc.h>
#include <colinux/kernel/manager.h>
#include <colinux/os/kernel/alloc.h>
#include <colinux/os/alloc.h>
#include <colinux/os/kernel/misc.h>
#include <colinux/arch/space.h>

#include "mmu.h"
#include "utils.h"

struct co_arch_guest_space {
	co_pfn_t      pml4_pfn;
	unsigned long tables;		/* page-table pages owned, including the PML4 */
};

#define CO_SPACE_LEVELS 4

/* Index at each level, outermost first. */
static unsigned long co_space_index(unsigned long long va, int level)
{
	switch (level) {
	case 0:  return CO_ARCH_PGD_INDEX(va);
	case 1:  return CO_ARCH_PUD_INDEX(va);
	case 2:  return CO_ARCH_PMD_INDEX(va);
	default: return CO_ARCH_PTE_INDEX(va);
	}
}

static co_rc_t co_space_new_table(co_manager_t* manager, co_pfn_t* pfn_out)
{
	unsigned long long* table;
	co_pfn_t pfn;
	co_rc_t rc;

	rc = co_os_get_page(manager, &pfn);
	if (!CO_OK(rc))
		return rc;

	table = co_os_map(manager, pfn);
	if (table == NULL) {
		co_os_put_page(manager, pfn);
		return CO_RC(ERROR);
	}

	/*
	 * A fresh table has to be all zeroes. co_os_get_page() makes no promise
	 * about contents, and a stale bit anywhere in here is a present entry
	 * pointing at an arbitrary physical page.
	 */
	co_memset(table, 0, CO_ARCH_PAGE_SIZE);
	co_os_unmap(manager, table, pfn);

	*pfn_out = pfn;
	return CO_RC(OK);
}

co_rc_t co_arch_guest_space_create(co_manager_t* manager,
				   co_arch_guest_space_t** space_out)
{
	co_arch_guest_space_t* space;
	co_rc_t rc;

	space = co_os_malloc(sizeof(*space));
	if (space == NULL)
		return CO_RC(OUT_OF_MEMORY);

	co_memset(space, 0, sizeof(*space));

	rc = co_space_new_table(manager, &space->pml4_pfn);
	if (!CO_OK(rc)) {
		co_os_free(space);
		return rc;
	}

	space->tables = 1;
	*space_out = space;

	return CO_RC(OK);
}

co_pa_t co_arch_guest_space_root(co_arch_guest_space_t* space)
{
	return ((co_pa_t)space->pml4_pfn) << CO_ARCH_PAGE_SHIFT;
}

unsigned long co_arch_guest_space_tables(co_arch_guest_space_t* space)
{
	return space->tables;
}

co_rc_t co_arch_guest_map(co_manager_t* manager,
			  co_arch_guest_space_t* space,
			  unsigned long long va,
			  co_pa_t pa,
			  unsigned long long flags)
{
	co_pfn_t pfn = space->pml4_pfn;
	int level;

	if (!CO_ARCH_VA_CANONICAL(va)) {
		co_debug_error("guest map: 0x%llx is not a canonical address", va);
		return CO_RC(INVALID_PARAMETER);
	}

	for (level = 0; level < CO_SPACE_LEVELS; level++) {
		unsigned long long* table;
		unsigned long long entry;
		unsigned long index = co_space_index(va, level);
		co_pfn_t next;
		co_rc_t rc;

		table = co_os_map(manager, pfn);
		if (table == NULL)
			return CO_RC(ERROR);

		if (level == CO_SPACE_LEVELS - 1) {
			table[index] = (pa & CO_ARCH_PAGE_MASK) | flags;
			co_os_unmap(manager, table, pfn);
			return CO_RC(OK);
		}

		entry = table[index];

		if (entry & _PAGE_PRESENT) {
			/*
			 * A large page here would mean the range is already
			 * described at 2 MB or 1 GB granularity and cannot also
			 * hold a 4 KB mapping. Refuse rather than corrupt it.
			 */
			if (entry & _PAGE_PSE) {
				co_os_unmap(manager, table, pfn);
				co_debug_error("guest map: 0x%llx crosses a large page at level %d",
					       va, level);
				return CO_RC(ERROR);
			}
			next = (co_pfn_t)((entry & CO_ARCH_PAGE_MASK & ~CO_ARCH_PAGE_NX)
					  >> CO_ARCH_PAGE_SHIFT);
			co_os_unmap(manager, table, pfn);
			pfn = next;
			continue;
		}

		/*
		 * The parent has to be unmapped before allocating, because
		 * co_os_map() may hold a single reusable window -- nesting two
		 * live mappings is not something the host allocator promises.
		 */
		co_os_unmap(manager, table, pfn);

		rc = co_space_new_table(manager, &next);
		if (!CO_OK(rc))
			return rc;

		table = co_os_map(manager, pfn);
		if (table == NULL) {
			co_os_put_page(manager, next);
			return CO_RC(ERROR);
		}

		/*
		 * Re-read rather than trusting the earlier value: this is the
		 * only window in which another mapping of the same address could
		 * have installed the table, and installing a second one would
		 * leak the first and split the address space in two.
		 */
		entry = table[index];
		if (entry & _PAGE_PRESENT) {
			co_os_unmap(manager, table, pfn);
			co_os_put_page(manager, next);
			pfn = (co_pfn_t)((entry & CO_ARCH_PAGE_MASK & ~CO_ARCH_PAGE_NX)
					 >> CO_ARCH_PAGE_SHIFT);
			continue;
		}

		table[index] = (((unsigned long long)next) << CO_ARCH_PAGE_SHIFT)
			       | _KERNPG_TABLE;
		co_os_unmap(manager, table, pfn);

		space->tables++;
		pfn = next;
	}

	return CO_RC(OK);
}

co_rc_t co_arch_guest_lookup(co_manager_t* manager,
			     co_arch_guest_space_t* space,
			     unsigned long long va,
			     co_pa_t* pa_out,
			     int* level_out)
{
	co_pfn_t pfn = space->pml4_pfn;
	int level;

	*pa_out    = 0;
	*level_out = -1;

	if (!CO_ARCH_VA_CANONICAL(va))
		return CO_RC(INVALID_PARAMETER);

	for (level = 0; level < CO_SPACE_LEVELS; level++) {
		unsigned long long* table;
		unsigned long long entry;
		unsigned long index = co_space_index(va, level);

		table = co_os_map(manager, pfn);
		if (table == NULL)
			return CO_RC(ERROR);

		entry = table[index];
		co_os_unmap(manager, table, pfn);

		if (!(entry & _PAGE_PRESENT)) {
			*level_out = level;
			return CO_RC(NOT_FOUND);
		}

		if (level == CO_SPACE_LEVELS - 1) {
			*pa_out = (co_pa_t)(entry & CO_ARCH_PAGE_MASK & ~CO_ARCH_PAGE_NX);
			return CO_RC(OK);
		}

		/* A large page terminates the walk and maps the range directly. */
		if (entry & _PAGE_PSE) {
			*pa_out    = (co_pa_t)(entry & CO_ARCH_PAGE_MASK & ~CO_ARCH_PAGE_NX);
			*level_out = level;
			return CO_RC(OK);
		}

		pfn = (co_pfn_t)((entry & CO_ARCH_PAGE_MASK & ~CO_ARCH_PAGE_NX)
				 >> CO_ARCH_PAGE_SHIFT);
	}

	return CO_RC(ERROR);
}

/*
 * Free every table below (and including) the one at pfn.
 *
 * level is the level of the table being freed, so the recursion stops before
 * descending into what PT entries point at -- those pages are the caller's.
 */
/*
 * Collect every page-table page in the space, outermost first.
 *
 * Same one-entry-at-a-time walk as the teardown below and for the same reason:
 * a 512-entry local is 4 KB and this recurses four deep, against a Windows x64
 * kernel stack that is not much bigger than that.
 */
static void co_space_collect_tables(co_manager_t* manager, co_pfn_t pfn, int level,
				    co_pfn_t* out, unsigned long* count,
				    unsigned long limit)
{
	int i;

	if (*count >= limit)
		return;
	out[(*count)++] = pfn;

	if (level >= CO_SPACE_LEVELS - 1)
		return;

	for (i = 0; i < 512; i++) {
		unsigned long long* table;
		unsigned long long entry;
		co_pfn_t child;

		table = co_os_map(manager, pfn);
		if (table == NULL)
			return;

		entry = table[i];
		co_os_unmap(manager, table, pfn);

		if (!(entry & _PAGE_PRESENT))
			continue;
		if (entry & _PAGE_PSE)
			continue;	/* maps memory, not a table */

		child = (co_pfn_t)((entry & CO_ARCH_PAGE_MASK & ~CO_ARCH_PAGE_NX)
				   >> CO_ARCH_PAGE_SHIFT);
		co_space_collect_tables(manager, child, level + 1, out, count, limit);
	}
}

/*
 * Give the space's own page tables addresses in its direct map.
 *
 * Linux reaches its page tables the same way it reaches anything else it knows
 * the physical address of -- through __va(). early_ioremap_pmd() is the first
 * to do it, with
 *
 *     pgd_t *base = __va(read_cr3_pa());
 *
 * and from there it walks down. Every one of those pages therefore has to be
 * readable at DIRECT_MAP + its physical address.
 *
 * They are not, by default, and cannot be by accident: the tables come from the
 * host's pool, so their physical addresses are wherever Windows had a page
 * free -- around 2 GB on this machine -- while the guest has been told it has
 * 128 MB. Nothing in the linear map built from that e820 covers them. The guest
 * ran thirty-three million instructions and then took a page fault reading
 * 0xffff88808054bff8, which is __va(CR3) + 0xff8: entry 511 of its own PML4.
 *
 * Mapping them creates more tables, which are themselves page-table pages that
 * may need the same treatment, so this repeats until a pass adds nothing.
 * It converges quickly -- the new tables cluster in the same few gigabytes of
 * host physical address space, so they share the tables just created.
 */
co_rc_t co_arch_guest_map_own_tables(co_manager_t* manager,
				     co_arch_guest_space_t* space,
				     unsigned long long direct_map_base,
				     unsigned long* mapped_out)
{
	enum { CO_SPACE_MAX_TABLES = 4096, CO_SPACE_MAX_PASSES = 8 };
	co_pfn_t* list;
	unsigned long total = 0;
	int pass;
	co_rc_t rc = CO_RC(OK);

	if (space == NULL)
		return CO_RC(INVALID_PARAMETER);

	list = co_os_malloc(sizeof(co_pfn_t) * CO_SPACE_MAX_TABLES);
	if (list == NULL)
		return CO_RC(OUT_OF_MEMORY);

	for (pass = 0; pass < CO_SPACE_MAX_PASSES; pass++) {
		unsigned long count = 0, added = 0, i;

		co_space_collect_tables(manager, space->pml4_pfn, 0, list, &count,
					CO_SPACE_MAX_TABLES);

		if (count >= CO_SPACE_MAX_TABLES) {
			co_debug_error("space: more than %d page tables; "
				       "cannot map them all into the direct map",
				       (int)CO_SPACE_MAX_TABLES);
			rc = CO_RC(ERROR);
			break;
		}

		for (i = 0; i < count; i++) {
			co_pa_t pa = ((co_pa_t)list[i]) << CO_ARCH_PAGE_SHIFT;
			unsigned long long va = direct_map_base + pa;
			co_pa_t found = 0;
			int level = -1;

			if (CO_OK(co_arch_guest_lookup(manager, space, va, &found, &level))
			    && found == pa)
				continue;

			rc = co_arch_guest_map(manager, space, va, pa, _KERNPG_TABLE);
			if (!CO_OK(rc))
				goto out;

			added++;
			total++;
		}

		if (added == 0)
			break;
	}

out:
	co_os_free(list);

	if (mapped_out)
		*mapped_out = total;

	return rc;
}

static void co_space_free_table(co_manager_t* manager, co_pfn_t pfn, int level,
				unsigned long* freed)
{
	int i;

	if (level < CO_SPACE_LEVELS - 1) {
		for (i = 0; i < 512; i++) {
			unsigned long long* table;
			unsigned long long entry;
			co_pfn_t child;

			/*
			 * One entry per mapping, rather than copying the table
			 * into a local and iterating that. A 512-entry local is
			 * 4 KB, and this recurses four deep -- 16 KB of kernel
			 * stack, against a Windows x64 kernel stack that is not
			 * much larger than that. Teardown is not hot, so paying
			 * 512 map/unmap pairs per table is the right trade.
			 */
			table = co_os_map(manager, pfn);
			if (table == NULL)
				break;

			entry = table[i];
			co_os_unmap(manager, table, pfn);

			if (!(entry & _PAGE_PRESENT))
				continue;
			if (entry & _PAGE_PSE)
				continue;	/* maps memory, not a table */

			child = (co_pfn_t)((entry & CO_ARCH_PAGE_MASK & ~CO_ARCH_PAGE_NX)
					   >> CO_ARCH_PAGE_SHIFT);
			co_space_free_table(manager, child, level + 1, freed);
		}
	}

	co_os_put_page(manager, pfn);
	(*freed)++;
}

void co_arch_guest_space_destroy(co_manager_t* manager, co_arch_guest_space_t* space)
{
	unsigned long freed = 0;

	if (space == NULL)
		return;

	co_space_free_table(manager, space->pml4_pfn, 0, &freed);

	if (freed != space->tables)
		co_debug_error("guest space: freed %ld tables, owned %ld",
			       freed, space->tables);

	co_os_free(space);
}

/*
 * Build a guest address space the shape a real one will be, then check it by
 * reading it back.
 *
 * The three regions are the ones M4 actually needs, chosen because they land in
 * three different PML4 slots and so exercise the builder's ability to create
 * whole subtrees rather than fill in one:
 *
 *   0xffffffff81000000  where the 7.1.5 kernel image is linked (_text)
 *   0xffff888000000000  PAGE_OFFSET, the base of the linear map of guest RAM
 *   the passage page's own host address, which is where the guest must map it
 *
 * Verification is a full walk per address, comparing against what was asked for.
 * A page table that is wrong is not a wrong answer; it is a triple fault with no
 * bugcheck, so the check has to happen while this is still only data. Nothing
 * here loads CR3.
 */
co_rc_t co_arch_test_space(co_manager_t* manager, co_arch_space_test_t* out)
{
	co_arch_guest_space_t* space = NULL;
	co_rc_t rc;
	int i;

	/* va, and how many consecutive pages to map there */
	static const struct { unsigned long long va; int pages; } regions[] = {
		{ 0xffffffff81000000ULL, 16 },	/* kernel image */
		{ 0xffff888000000000ULL, 16 },	/* linear map of guest RAM */
		{ 0xffffe9a000000000ULL,  4 },	/* the provisional guest window */
	};
	const int region_count = sizeof(regions) / sizeof(regions[0]);

	co_memset(out, 0, sizeof(*out));
	out->supported = PTRUE;

	rc = co_arch_guest_space_create(manager, &space);
	if (!CO_OK(rc)) {
		co_debug_error("space test: could not create a space (rc %x)", (int)rc);
		return rc;
	}

	out->root = co_arch_guest_space_root(space);

	/*
	 * Map each page to a physical address derived from its virtual one, so a
	 * mixed-up entry cannot coincidentally verify. A constant, or the same pfn
	 * everywhere, would let an off-by-one in the index arithmetic pass.
	 */
	for (i = 0; i < region_count; i++) {
		int page;

		for (page = 0; page < regions[i].pages; page++) {
			unsigned long long va = regions[i].va
					      + ((unsigned long long)page << CO_ARCH_PAGE_SHIFT);
			co_pa_t pa = (co_pa_t)(((i + 1) * 0x100000ULL)
					       + ((unsigned long long)page << CO_ARCH_PAGE_SHIFT));

			rc = co_arch_guest_map(manager, space, va, pa, _KERNPG_TABLE);
			if (!CO_OK(rc)) {
				co_debug_error("space test: mapping 0x%llx failed (rc %x)",
					       va, (int)rc);
				co_arch_guest_space_destroy(manager, space);
				return rc;
			}
			out->mapped++;
		}
	}

	out->tables = co_arch_guest_space_tables(space);

	for (i = 0; i < region_count; i++) {
		int page;

		for (page = 0; page < regions[i].pages; page++) {
			unsigned long long va = regions[i].va
					      + ((unsigned long long)page << CO_ARCH_PAGE_SHIFT);
			co_pa_t expect = (co_pa_t)(((i + 1) * 0x100000ULL)
						   + ((unsigned long long)page << CO_ARCH_PAGE_SHIFT));
			co_pa_t got = 0;
			int level = -1;

			rc = co_arch_guest_lookup(manager, space, va, &got, &level);
			if (CO_OK(rc) && got == expect) {
				out->verified++;
			} else {
				if (out->first_bad_va == 0) {
					out->first_bad_va     = va;
					out->first_bad_expect = expect;
					out->first_bad_got    = got;
				}
				out->mismatched++;
			}
		}
	}

	/*
	 * And an address that was never mapped. Without this the test would pass
	 * just as well against a walker that returned success unconditionally.
	 */
	{
		co_pa_t got = 0;
		int level = -1;

		rc = co_arch_guest_lookup(manager, space, 0xffffc90000000000ULL, &got, &level);
		out->unmapped_reported = (!CO_OK(rc)) ? PTRUE : PFALSE;
		out->unmapped_level    = level;
	}

	co_arch_guest_space_destroy(manager, space);

	out->succeeded = (out->mismatched == 0 &&
			  out->verified == out->mapped &&
			  out->mapped > 0 &&
			  out->unmapped_reported) ? PTRUE : PFALSE;

	return CO_RC(OK);
}
