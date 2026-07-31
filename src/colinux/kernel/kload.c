/*
 * This source code is a part of coLinux source package.
 *
 * The code is licensed under the GPL. See the COPYING file at
 * the root directory.
 */

/*
 * Loading a real kernel image into a guest address space.
 *
 * Everything up to here has run guests that were a few bytes of hand-written
 * assembly. This puts an actual vmlinux where it expects to be: thirty-odd
 * megabytes of sections at their link addresses, in a space built page by page,
 * with the pages allocated on demand as the sections are written.
 *
 * It is deliberately separate from the monitor's own load path. The monitor
 * loads into pseudo-physical memory it owns and needs a monitor instance, a
 * pseudo-physical map and a running guest; none of that exists yet and building
 * it is M4's real work. This is the narrow version: allocate a page when a write
 * lands on one that is absent, map it into the space, copy. Enough to find out
 * whether an image the size of a kernel maps correctly before anything depends
 * on it.
 *
 * One space at a time, held in a file static. This is bring-up scaffolding, not
 * something a second caller should be able to reach concurrently.
 */

/* GCC's own freestanding stddef.h, for size_t as a pointer-sized integer. */
#include <stddef.h>

#include <colinux/common/common.h>
#include <colinux/common/debug.h>
#include <colinux/common/libc.h>
#include <colinux/common/ioctl.h>
#include <colinux/os/kernel/alloc.h>
#include <colinux/os/kernel/misc.h>
#include <colinux/arch/mmu.h>
#include <colinux/arch/space.h>

#include "manager.h"
#include "kload.h"

static co_arch_guest_space_t* kload_space;
static unsigned long long     kload_min_va;
static unsigned long long     kload_max_va;
static unsigned long	      kload_pages;
static unsigned long	      kload_chunks;
static unsigned long long     kload_ram_bytes;
static unsigned long long     kload_text_phys;
static unsigned long long     kload_end_phys;
static unsigned long	      kload_ram_pages;

/*
 * Pages are found again at teardown by walking the address range rather than
 * remembering them in a list. The range is known, every present leaf in it is a
 * page this code allocated, and a list can disagree with the tables in exactly
 * one direction -- freeing something still referenced.
 */
static void kload_release_pages(co_manager_t* manager)
{
	unsigned long long va;

	if (kload_space == NULL)
		return;

	for (va = kload_min_va; va < kload_max_va; va += CO_ARCH_PAGE_SIZE) {
		co_pa_t pa = 0;
		int level = -1;

		if (CO_OK(co_arch_guest_lookup(manager, kload_space, va, &pa, &level)) && pa)
			co_os_put_page(manager, (co_pfn_t)(pa >> CO_ARCH_PAGE_SHIFT));
	}

	/*
	 * The linear map, skipping the stretch that is the image. Those pages have
	 * two virtual addresses and were already released above; freeing them again
	 * here would be a double free of a page the host may have handed out in the
	 * meantime, which is far worse than leaking one.
	 */
	for (va = 0; va < kload_ram_bytes; va += CO_ARCH_PAGE_SIZE) {
		co_pa_t pa = 0;
		int level = -1;

		if (va >= kload_text_phys && va < kload_end_phys)
			continue;

		if (CO_OK(co_arch_guest_lookup(manager, kload_space,
					       CO_ARCH_DIRECT_MAP + va, &pa, &level)) && pa)
			co_os_put_page(manager, (co_pfn_t)(pa >> CO_ARCH_PAGE_SHIFT));
	}
}

void co_kload_free(co_manager_t* manager)
{
	if (kload_space == NULL)
		return;

	kload_release_pages(manager);
	co_arch_guest_space_destroy(manager, kload_space);

	kload_space     = NULL;
	kload_pages     = 0;
	kload_chunks    = 0;
	kload_ram_bytes = 0;
	kload_ram_pages = 0;
}

co_rc_t co_kload_begin(co_manager_t* manager, unsigned long long min_va,
		       unsigned long long max_va)
{
	co_rc_t rc;

	co_kload_free(manager);

	if (min_va >= max_va || !CO_ARCH_VA_CANONICAL(min_va) || !CO_ARCH_VA_CANONICAL(max_va - 1))
		return CO_RC(INVALID_PARAMETER);

	rc = co_arch_guest_space_create(manager, &kload_space);
	if (!CO_OK(rc))
		return rc;

	kload_min_va = min_va & CO_ARCH_PAGE_MASK;
	kload_max_va = (max_va + CO_ARCH_PAGE_SIZE - 1) & CO_ARCH_PAGE_MASK;
	kload_pages  = 0;
	kload_chunks = 0;

	co_debug("kload: space for 0x%llx..0x%llx (%lld pages)",
		 kload_min_va, kload_max_va,
		 (kload_max_va - kload_min_va) >> CO_ARCH_PAGE_SHIFT);

	return CO_RC(OK);
}

/* The page backing va, allocating and mapping it if this is its first touch. */
static co_rc_t kload_page_for(co_manager_t* manager, unsigned long long va,
			      co_pfn_t* pfn_out)
{
	unsigned long long page_va = va & CO_ARCH_PAGE_MASK;
	co_pa_t pa = 0;
	co_pfn_t pfn;
	int level = -1;
	co_rc_t rc;

	if (CO_OK(co_arch_guest_lookup(manager, kload_space, page_va, &pa, &level)) && pa) {
		*pfn_out = (co_pfn_t)(pa >> CO_ARCH_PAGE_SHIFT);
		return CO_RC(OK);
	}

	rc = co_os_get_page(manager, &pfn);
	if (!CO_OK(rc))
		return rc;

	/*
	 * Zero before mapping, not after. A section that only partly fills its
	 * last page would otherwise leave whatever the host had there visible to
	 * the guest, and .bss is nothing but that case.
	 */
	{
		void* p = co_os_map(manager, pfn);

		if (p == NULL) {
			co_os_put_page(manager, pfn);
			return CO_RC(ERROR);
		}
		co_memset(p, 0, CO_ARCH_PAGE_SIZE);
		co_os_unmap(manager, p, pfn);
	}

	rc = co_arch_guest_map(manager, kload_space, page_va,
			       ((co_pa_t)pfn) << CO_ARCH_PAGE_SHIFT, _KERNPG_TABLE);
	if (!CO_OK(rc)) {
		co_os_put_page(manager, pfn);
		return rc;
	}

	kload_pages++;
	*pfn_out = pfn;

	return CO_RC(OK);
}

co_rc_t co_kload_chunk(co_manager_t* manager, unsigned long long va,
		       const unsigned char* data, unsigned long size, bool_t zero)
{
	if (kload_space == NULL)
		return CO_RC(ERROR);

	if (va < kload_min_va || va + size > kload_max_va) {
		co_debug_error("kload: chunk 0x%llx+0x%lx is outside 0x%llx..0x%llx",
			       va, size, kload_min_va, kload_max_va);
		return CO_RC(INVALID_PARAMETER);
	}

	while (size) {
		unsigned long offset = (unsigned long)(va & ~CO_ARCH_PAGE_MASK);
		unsigned long part   = CO_ARCH_PAGE_SIZE - offset;
		co_pfn_t pfn;
		unsigned char* p;
		co_rc_t rc;

		if (part > size)
			part = size;

		rc = kload_page_for(manager, va, &pfn);
		if (!CO_OK(rc))
			return rc;

		/* A zeroing chunk needs the page to exist but nothing written. */
		if (!zero) {
			p = co_os_map(manager, pfn);
			if (p == NULL)
				return CO_RC(ERROR);

			co_memcpy(p + offset, data, part);
			co_os_unmap(manager, p, pfn);
			data += part;
		}

		va   += part;
		size -= part;
	}

	kload_chunks++;

	return CO_RC(OK);
}

/*
 * A checksum of what is actually in guest memory, read back through the guest's
 * own page tables.
 *
 * The point is not integrity in transit -- it is that the bytes are reachable at
 * the address the kernel was linked for, by the same walk the hardware would do.
 * A loader that wrote everything correctly into pages nobody mapped would pass
 * every other check and fail here.
 */
co_rc_t co_kload_verify(co_manager_t* manager, unsigned long long va,
			unsigned long size, unsigned long long* sum_out)
{
	unsigned long long sum = 1469598103934665603ULL;	/* FNV-1a offset basis */

	if (kload_space == NULL)
		return CO_RC(ERROR);

	while (size) {
		unsigned long offset = (unsigned long)(va & ~CO_ARCH_PAGE_MASK);
		unsigned long part   = CO_ARCH_PAGE_SIZE - offset;
		co_pa_t pa = 0;
		int level = -1;
		unsigned char* p;
		unsigned long i;

		if (part > size)
			part = size;

		if (!CO_OK(co_arch_guest_lookup(manager, kload_space, va, &pa, &level)) || !pa) {
			co_debug_error("kload: verify: 0x%llx is not mapped (level %d)", va, level);
			return CO_RC(NOT_FOUND);
		}

		p = co_os_map(manager, (co_pfn_t)(pa >> CO_ARCH_PAGE_SHIFT));
		if (p == NULL)
			return CO_RC(ERROR);

		for (i = 0; i < part; i++) {
			sum ^= p[offset + i];
			sum *= 1099511628211ULL;		/* FNV-1a prime */
		}

		co_os_unmap(manager, p, (co_pfn_t)(pa >> CO_ARCH_PAGE_SHIFT));

		va   += part;
		size -= part;
	}

	*sum_out = sum;

	return CO_RC(OK);
}

co_arch_guest_space_t* co_kload_space(void)
{
	return kload_space;
}

unsigned long co_kload_pages(void)
{
	return kload_pages;
}

unsigned long co_kload_chunks(void)
{
	return kload_chunks;
}

/*
 * Give the guest some physical memory, and a linear map of it.
 *
 * Loading the image is not enough to reach start_kernel. The kernel thinks in
 * physical addresses: setup_arch reads an e820 map, hands the ranges to
 * memblock, and from then on every allocation is a physical address turned back
 * into a virtual one with __va(), which is PAGE_OFFSET + phys. If that linear
 * map does not exist, the first allocation the kernel dereferences is a fault --
 * and by that point in setup_arch it has already replaced our IDT, so the fault
 * is unreportable.
 *
 * So: decide a physical layout, back every page of it, and map the whole thing
 * at PAGE_OFFSET.
 *
 * The image is already mapped at its link addresses, and those same pages have
 * to appear in the linear map too -- one physical page, two virtual addresses,
 * which is exactly what a real kernel has. Their physical address follows from
 * the link address, since phys_base is zero and the kernel is linked at
 * __START_KERNEL_map + CONFIG_PHYSICAL_START.
 */
co_rc_t co_kload_build_ram(co_manager_t* manager, unsigned long long ram_bytes,
			   unsigned long long text_va, unsigned long long end_va)
{
	unsigned long long phys;
	unsigned long long text_phys, end_phys;

	if (kload_space == NULL)
		return CO_RC(ERROR);

	if (text_va < CO_ARCH_KERNEL_MAP || end_va <= text_va)
		return CO_RC(INVALID_PARAMETER);

	text_phys = (text_va - CO_ARCH_KERNEL_MAP) & CO_ARCH_PAGE_MASK;
	end_phys  = (end_va - CO_ARCH_KERNEL_MAP + CO_ARCH_PAGE_SIZE - 1) & CO_ARCH_PAGE_MASK;

	if (end_phys > ram_bytes) {
		co_debug_error("kload: image ends at guest phys 0x%llx, past %lld MB of RAM",
			       end_phys, ram_bytes >> 20);
		return CO_RC(INVALID_PARAMETER);
	}

	kload_ram_bytes = ram_bytes;
	kload_text_phys = text_phys;
	kload_end_phys  = end_phys;

	for (phys = 0; phys < ram_bytes; phys += CO_ARCH_PAGE_SIZE) {
		unsigned long long lin_va = CO_ARCH_DIRECT_MAP + phys;
		co_pa_t pa = 0;
		int level = -1;
		co_rc_t rc;

		if (phys >= text_phys && phys < end_phys) {
			/*
			 * Part of the image. It already has a host page behind it;
			 * find that page and give it its second virtual address
			 * rather than allocating a new one, or the linear map would
			 * show a blank copy of the kernel.
			 */
			unsigned long long kva = CO_ARCH_KERNEL_MAP + phys;

			if (!CO_OK(co_arch_guest_lookup(manager, kload_space, kva, &pa, &level))
			    || !pa) {
				/* a gap between sections -- back it like ordinary RAM */
				pa = 0;
			}
		}

		if (!pa) {
			co_pfn_t pfn;
			void* p;

			rc = co_os_get_page(manager, &pfn);
			if (!CO_OK(rc)) {
				co_debug_error("kload: out of pages at guest phys 0x%llx", phys);
				return rc;
			}

			p = co_os_map(manager, pfn);
			if (p == NULL) {
				co_os_put_page(manager, pfn);
				return CO_RC(ERROR);
			}
			co_memset(p, 0, CO_ARCH_PAGE_SIZE);
			co_os_unmap(manager, p, pfn);

			pa = ((co_pa_t)pfn) << CO_ARCH_PAGE_SHIFT;
			kload_ram_pages++;
		}

		rc = co_arch_guest_map(manager, kload_space, lin_va, pa, _KERNPG_TABLE);
		if (!CO_OK(rc))
			return rc;
	}

	co_debug("kload: %lld MB of guest RAM, %ld pages allocated for it",
		 ram_bytes >> 20, kload_ram_pages);

	/*
	 * Not calling co_arch_guest_map_own_tables() here, deliberately.
	 *
	 * It maps each page-table page at DIRECT_MAP + its *host* physical
	 * address, which collides with the guest's own linear map for any table
	 * that happens to land below ram_bytes of host physical memory: the
	 * table's mapping silently replaces a RAM page's, and teardown then
	 * frees that page while the space destructor frees it again. A double
	 * free of a page the host has since handed to someone else, which is
	 * how the pool got corrupted.
	 *
	 * The deeper problem is that it would not be enough even without the
	 * collision -- see the note on __va() in colinux/arch/space.h.
	 */

	return CO_RC(OK);
}

unsigned long co_kload_ram_pages(void)
{
	return kload_ram_pages;
}
