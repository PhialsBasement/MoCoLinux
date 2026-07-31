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
 * The guest's physical memory: one physically contiguous block, and the rule
 * that guest physical address N is host physical address kload_block_pa + N.
 *
 * Scattered per-page allocations cannot work, and the reason is not efficiency.
 * Linux reads its own page tables and calls __va() on what it finds -- that is
 * how early_ioremap_pmd() walks down from __va(read_cr3_pa()) -- so the numbers
 * inside a page table entry have to mean the same thing to the guest as to the
 * hardware. The hardware requires host physical addresses. Therefore the guest's
 * physical addresses have to *be* host physical addresses, for every page it
 * can reach: its RAM, its image, and the page tables themselves.
 *
 * One contiguous block gives exactly that, with a single e820 entry and a
 * direct map that is one identity-offset range. It also removes the whole
 * class of teardown bug: there is one allocation to free, not thirty thousand.
 */
static void*		      kload_block;	/* host virtual, from the allocator */
static co_pa_t		      kload_block_pa;	/* host physical, == guest physical 0 */
static unsigned long long     kload_block_bytes;
static unsigned long long     kload_table_top;	/* tables grow down from here */

/*
 * Page tables come out of the top of the block and that region is reported to
 * the guest as reserved.
 *
 * They have to be inside the block so __va() reaches them, and they have to be
 * memory the guest will not reuse -- these are tables the host built and Linux
 * knows nothing about, so anything it allocated over them would be overwriting
 * its own address space.
 */
#define CO_KLOAD_TABLE_BYTES	(4ULL << 20)

/*
 * The host virtual address of a frame inside the block, or NULL.
 *
 * The block already has one system mapping, from the allocator. Mapping its
 * pages a second time with MmMapIoSpace -- which is what co_os_map does --
 * creates an alias with a different cache attribute over memory that is
 * ordinary RAM, and Driver Verifier stops the machine for it with
 * "a device driver attempting to corrupt the system has been caught". It is
 * right to: aliased cache attributes on RAM are architecturally undefined.
 *
 * There is nothing to map. The address is arithmetic.
 */
void* co_kload_frame_va(co_pfn_t pfn)
{
	co_pa_t pa = ((co_pa_t)pfn) << CO_ARCH_PAGE_SHIFT;

	if (kload_block == NULL || pa < kload_block_pa ||
	    pa >= kload_block_pa + kload_block_bytes)
		return NULL;

	return (void*)((char*)kload_block + (unsigned long)(pa - kload_block_pa));
}

co_pa_t co_kload_block_pa(void)
{
	return kload_block_pa;
}

unsigned long long co_kload_usable_bytes(void)
{
	return kload_block_bytes - CO_KLOAD_TABLE_BYTES;
}

/*
 * A frame from the block, by guest physical offset. Zeroed, because a page
 * table with a stale bit in it is a present entry pointing anywhere.
 */
static co_rc_t kload_frame_at(co_manager_t* manager, unsigned long long offset,
			      co_pfn_t* pfn_out)
{
	co_pa_t pa;

	if (kload_block == NULL || offset + CO_ARCH_PAGE_SIZE > kload_block_bytes)
		return CO_RC(INVALID_PARAMETER);

	pa = kload_block_pa + offset;
	*pfn_out = (co_pfn_t)(pa >> CO_ARCH_PAGE_SHIFT);

	return CO_RC(OK);
}

/* A page-table frame, taken from the reserved region at the top of the block. */
co_rc_t co_kload_table_frame(co_manager_t* manager, co_pfn_t* pfn_out)
{
	unsigned long long* table;
	co_pfn_t pfn;
	co_rc_t rc;

	if (kload_block == NULL)
		return CO_RC(ERROR);

	if (kload_table_top < kload_block_bytes - CO_KLOAD_TABLE_BYTES + CO_ARCH_PAGE_SIZE) {
		co_debug_error("kload: out of page-table space in the guest block");
		return CO_RC(OUT_OF_MEMORY);
	}

	kload_table_top -= CO_ARCH_PAGE_SIZE;

	rc = kload_frame_at(manager, kload_table_top, &pfn);
	if (!CO_OK(rc))
		return rc;

	table = co_kload_frame_va(pfn);
	if (table == NULL)
		return CO_RC(ERROR);
	co_memset(table, 0, CO_ARCH_PAGE_SIZE);

	*pfn_out = pfn;
	return CO_RC(OK);
}

/*
 * Pages are found again at teardown by walking the address range rather than
 * remembering them in a list. The range is known, every present leaf in it is a
 * page this code allocated, and a list can disagree with the tables in exactly
 * one direction -- freeing something still referenced.
 */
static void kload_release_pages(co_manager_t* manager)
{
	/*
	 * Nothing to walk any more.
	 *
	 * Every page the guest can see -- RAM, image, page tables -- is part of
	 * one contiguous allocation, so teardown frees that and is done. The
	 * previous version walked the address ranges and freed each present
	 * leaf, taking care not to visit the image twice because it has two
	 * virtual addresses; every mapping added since had to be checked
	 * against that walk, and the one that was not was a page freed twice
	 * and then handed to something else by the host.
	 */
	if (kload_block == NULL)
		return;

	co_os_free_contiguous_pages(kload_block,
				    (unsigned int)(kload_block_bytes >> CO_ARCH_PAGE_SHIFT));

	kload_block       = NULL;
	kload_block_pa    = 0;
	kload_block_bytes = 0;
	kload_table_top   = 0;
}

void co_kload_free(co_manager_t* manager)
{
	if (kload_space == NULL)
		return;

	/*
	 * The space first, then the memory it lives in.
	 *
	 * The page tables are inside the block now, so freeing the block before
	 * walking them is a use-after-free of the whole address space -- and it
	 * is not subtle: the destructor reads the PML4, which is the last page
	 * of the allocation, so Driver Verifier caught it every single time at
	 * exactly that frame.
	 */
	co_arch_guest_space_destroy(manager, kload_space);
	co_arch_guest_space_set_frame_source(NULL, NULL);
	kload_release_pages(manager);

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

	/*
	 * The guest's physical memory, allocated once and contiguously so that
	 * guest physical N is host physical block_pa + N. Everything the guest
	 * can reach comes out of this.
	 */
	kload_block_bytes = CO_KLOAD_RAM_BYTES;
	kload_block = co_os_alloc_contiguous_pages(
			(unsigned int)(kload_block_bytes >> CO_ARCH_PAGE_SHIFT));
	if (kload_block == NULL) {
		co_debug_error("kload: could not get %lld MB of contiguous memory",
			       kload_block_bytes >> 20);
		return CO_RC(OUT_OF_MEMORY);
	}

	kload_block_pa  = co_os_virt_to_phys(kload_block);
	kload_table_top = kload_block_bytes;

	co_debug("kload: guest RAM is %lld MB at host physical 0x%llx",
		 kload_block_bytes >> 20, (unsigned long long)kload_block_pa);

	/* page tables out of the block too, or Linux cannot walk them */
	co_arch_guest_space_set_frame_source(co_kload_table_frame, co_kload_frame_va);

	rc = co_arch_guest_space_create(manager, &kload_space);
	if (!CO_OK(rc)) {
		co_arch_guest_space_set_frame_source(NULL, NULL);
		kload_release_pages(manager);
		return rc;
	}

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

	/*
	 * The frame is not chosen, it is implied. The image is linked at
	 * __START_KERNEL_map + its physical address, so the page backing kernel
	 * virtual address V has to be guest physical V - __START_KERNEL_map --
	 * that is what __pa() will compute later, and what the linear map has to
	 * agree with.
	 */
	if (page_va < CO_ARCH_KERNEL_MAP)
		return CO_RC(INVALID_PARAMETER);

	rc = kload_frame_at(manager, page_va - CO_ARCH_KERNEL_MAP, &pfn);
	if (!CO_OK(rc)) {
		co_debug_error("kload: 0x%llx is past the end of guest RAM", page_va);
		return rc;
	}

	/*
	 * Zero before mapping, not after. A section that only partly fills its
	 * last page would otherwise leave whatever the host had there visible to
	 * the guest, and .bss is nothing but that case.
	 */
	{
		void* p = co_kload_frame_va(pfn);

		if (p == NULL)
			return CO_RC(ERROR);
		co_memset(p, 0, CO_ARCH_PAGE_SIZE);
	}

	rc = co_arch_guest_map(manager, kload_space, page_va,
			       ((co_pa_t)pfn) << CO_ARCH_PAGE_SHIFT, _KERNPG_TABLE);
	if (!CO_OK(rc)) {
		/* the frame belongs to the block; nothing to give back */
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
			p = co_kload_frame_va(pfn);
			if (p == NULL)
				return CO_RC(ERROR);

			co_memcpy(p + offset, data, part);

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

		p = co_kload_frame_va((co_pfn_t)(pa >> CO_ARCH_PAGE_SHIFT));
		if (p == NULL)
			return CO_RC(ERROR);

		for (i = 0; i < part; i++) {
			sum ^= p[offset + i];
			sum *= 1099511628211ULL;		/* FNV-1a prime */
		}



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

	/*
	 * The direct map: one identity-offset range over the whole block.
	 *
	 * __va(pa) is PAGE_OFFSET + pa, and pa is now a host physical address
	 * inside the block, so this is the mapping that makes __va() mean
	 * something -- for RAM, for the image, and for the page tables in the
	 * reserved region at the top, which is why it covers the block rather
	 * than just the usable part.
	 */
	for (phys = 0; phys < kload_block_bytes; phys += CO_ARCH_PAGE_SIZE) {
		co_pa_t pa = kload_block_pa + phys;
		co_rc_t rc = co_arch_guest_map(manager, kload_space,
					       CO_ARCH_DIRECT_MAP + pa, pa,
					       _KERNPG_TABLE);

		if (!CO_OK(rc))
			return rc;

		kload_ram_pages++;
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
