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
#include "console.h"
#include "net.h"

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
 * The guest's physical memory: a handful of physically contiguous blocks, and
 * the rule that guest physical addresses *are* host physical addresses.
 *
 * Scattered per-page allocations cannot work, and the reason is not efficiency.
 * Linux reads its own page tables and calls __va() on what it finds -- that is
 * how early_ioremap_pmd() walks down from __va(read_cr3_pa()) -- so the numbers
 * inside a page table entry have to mean the same thing to the guest as to the
 * hardware. The hardware requires host physical addresses. Therefore the guest's
 * physical addresses have to *be* host physical addresses, for every page it
 * can reach: its RAM, its image, and the page tables themselves.
 *
 * What identity does not require is that the memory be one piece. Linux is
 * entirely happy with a fragmented e820 -- real machines have holes -- so each
 * block is simply one more usable range, described where it really is. That
 * matters because one contiguous allocation of over a hundred megabytes is the
 * first thing a host that has been up for a while refuses; a 32 MB fallback was
 * tried and is strictly worse than failing, because the image cannot fit and
 * nothing said so.
 *
 * Two things still have to be contiguous, and they set the floor:
 *
 *   - the image. __pa() is linear -- x - __START_KERNEL_map + phys_base -- so
 *     the image's physical placement is one unbroken run, about 40 MB for this
 *     kernel. phys_base is ours to choose, so the image starts at block 0's
 *     base rather than 16 MB into it as CONFIG_PHYSICAL_START would imply.
 *   - the page-table region, which lives at the top of block 0.
 *
 * Block 0 is therefore exactly image + tables, and everything else is ordinary
 * RAM in whatever pieces the host can spare.
 */
typedef struct {
	void*		   raw;		/* exactly what the allocator returned */
	void*		   va;		/* the 2 MB-aligned view inside it */
	co_pa_t		   pa;		/* host physical of that view */
	unsigned long long bytes;	/* size of the aligned view, 2 MB multiple */
} co_kload_block_t;

static co_kload_block_t	      kload_block[CO_KLOAD_MAX_BLOCKS];
static int		      kload_block_count;
static unsigned long long     kload_phys_base;	/* what the guest's __pa() adds */
static unsigned long long     kload_table_top;	/* block 0 offset; tables grow down */

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
	int i;

	for (i = 0; i < kload_block_count; i++) {
		if (pa >= kload_block[i].pa &&
		    pa < kload_block[i].pa + kload_block[i].bytes)
			return (void*)((char*)kload_block[i].va
				       + (unsigned long)(pa - kload_block[i].pa));
	}

	return NULL;
}

unsigned long long co_kload_phys_base(void)
{
	return kload_phys_base;
}

int co_kload_range_count(void)
{
	return kload_block_count;
}

/*
 * One block as the guest should hear about it: a usable range, and for block 0
 * the reserved page-table region above it. The e820 is built from exactly this.
 */
void co_kload_range(int i, unsigned long long* pa, unsigned long long* usable,
		    unsigned long long* reserved)
{
	*pa = *usable = *reserved = 0;

	if (i < 0 || i >= kload_block_count)
		return;

	*pa	  = kload_block[i].pa;
	*usable	  = kload_block[i].bytes;
	*reserved = 0;

	if (i == 0) {
		*usable	 -= CO_KLOAD_TABLE_BYTES;
		*reserved = CO_KLOAD_TABLE_BYTES;
	}
}

/* The host physical address the guest's own __pa() would compute for kva. */
static co_pa_t kload_kva_pa(unsigned long long kva)
{
	return (co_pa_t)(kva - CO_ARCH_KERNEL_MAP + kload_phys_base);
}

/*
 * One more contiguous block, 2 MB aligned by overallocation because the
 * allocator promises nothing beyond a page, and a base that is merely page
 * aligned puts bits into a PSE entry's reserved field. The original pointer is
 * what gets freed.
 *
 * Anywhere in physical memory, including above 4 GB.
 *
 * There was a ceiling here for a while and it was the wrong layer. A block at
 * 0x104200000 appeared in the guest's e820 and then vanished from the kernel's
 * own map a moment later -- last_pfn 0x106200 -> 0x2bc00 -- and the answer to
 * that was to stop allocating up there. But nothing about this is an
 * addressing limit; the driver is 64-bit throughout. What removed the block
 * was mtrr_trim_uncached_memory(), discarding RAM the host's MTRRs do not
 * describe as write-back, and the guest is told not to do that instead
 * (disable_mtrr_trim, see the command line in elf_load.c).
 *
 * The ceiling also cost more than it saved. It halved the space this search
 * runs in, on the one allocation that still needs a long unbroken run, and on
 * a host that has been up for a day the image block then cannot be found at
 * all -- which is a worse failure than the one it was avoiding.
 */
static co_kload_block_t* kload_block_alloc(unsigned long long bytes)
{
	co_kload_block_t* b;
	void* raw;

	if (kload_block_count >= CO_KLOAD_MAX_BLOCKS)
		return NULL;

	raw = co_os_alloc_contiguous_pages(
			(unsigned int)((bytes + CO_ARCH_PMD_SIZE)
				       >> CO_ARCH_PAGE_SHIFT));
	if (raw == NULL)
		return NULL;

	b = &kload_block[kload_block_count++];
	b->raw	 = raw;
	b->bytes = bytes;

	{
		co_pa_t raw_pa = co_os_virt_to_phys(raw);
		unsigned long long adjust =
			(CO_ARCH_PMD_SIZE - (raw_pa & (CO_ARCH_PMD_SIZE - 1)))
			& (CO_ARCH_PMD_SIZE - 1);

		b->va = (void*)((char*)raw + (unsigned long)adjust);
		b->pa = raw_pa + adjust;
	}

	return b;
}

/* A page-table frame, taken from the reserved region at the top of block 0. */
co_rc_t co_kload_table_frame(co_manager_t* manager, co_pfn_t* pfn_out)
{
	unsigned long long* table;
	co_pfn_t pfn;

	if (kload_block_count == 0)
		return CO_RC(ERROR);

	if (kload_table_top < kload_block[0].bytes - CO_KLOAD_TABLE_BYTES
			      + CO_ARCH_PAGE_SIZE) {
		co_debug_error("kload: out of page-table space in the guest block");
		return CO_RC(OUT_OF_MEMORY);
	}

	kload_table_top -= CO_ARCH_PAGE_SIZE;

	pfn = (co_pfn_t)((kload_block[0].pa + kload_table_top) >> CO_ARCH_PAGE_SHIFT);

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
	 * one of these allocations, so teardown frees each and is done. The
	 * previous version walked the address ranges and freed each present
	 * leaf, taking care not to visit the image twice because it has two
	 * virtual addresses; every mapping added since had to be checked
	 * against that walk, and the one that was not was a page freed twice
	 * and then handed to something else by the host.
	 */
	int i;

	for (i = 0; i < kload_block_count; i++) {
		if (kload_block[i].raw == NULL)
			continue;

		co_os_free_contiguous_pages(kload_block[i].raw,
					    (unsigned int)((kload_block[i].bytes
							    + CO_ARCH_PMD_SIZE)
							   >> CO_ARCH_PAGE_SHIFT));
		kload_block[i].raw   = NULL;
		kload_block[i].va    = NULL;
		kload_block[i].pa    = 0;
		kload_block[i].bytes = 0;
	}

	kload_block_count = 0;
	kload_phys_base	  = 0;
	kload_table_top	  = 0;
}

void co_kload_free(co_manager_t* manager)
{
	co_arch_guest_space_t* space = kload_space;

	if (space == NULL && kload_block_count == 0)
		return;

	/*
	 * Retire the cross-process readers here, not at the call sites.
	 *
	 * The console pump and the network bridge are separate processes that
	 * reach guest memory by walking these tables, so the address they walk
	 * from has to be taken away -- under their own locks, which is what
	 * makes an in-flight walk finish before anything is freed -- before this
	 * function touches anything.
	 *
	 * It was done at the call sites, and that was wrong. Four of them in
	 * manager.c retired first and were audited as if they were all of them;
	 * the fifth is co_kload_begin below, which frees the previous run's
	 * space at the start of a new one and retired nothing. A bridge polling
	 * across a re-run therefore walked freed tables: bugcheck 0xD5 under
	 * Driver Verifier, at co_arch_guest_lookup, reached through
	 * co_net_dump -> co_kload_read. Putting it here means a caller cannot
	 * forget, including one added later.
	 */
	co_console_set_address(0);
	co_net_set_address(0);

	/*
	 * Clear the global before destroying what it points at.
	 *
	 * Every reader guards with `if (kload_space == NULL) return`, and that
	 * check is worth nothing while the pointer still names freed memory:
	 * the reader passes the test and hands the dangling space to
	 * co_arch_guest_lookup. Publishing NULL first closes the window to
	 * nothing, because the retirement above has already stopped the only
	 * callers that could be inside a walk.
	 */
	kload_space = NULL;

	/*
	 * The space first, then the memory it lives in.
	 *
	 * The page tables are inside the block now, so freeing the block before
	 * walking them is a use-after-free of the whole address space -- and it
	 * is not subtle: the destructor reads the PML4, which is the last page
	 * of the allocation, so Driver Verifier caught it every single time at
	 * exactly that frame.
	 */
	if (space != NULL)
		co_arch_guest_space_destroy(manager, space);
	co_arch_guest_space_set_frame_source(NULL, NULL);
	kload_release_pages(manager);

	kload_pages     = 0;
	kload_chunks    = 0;
	kload_ram_bytes = 0;
	kload_ram_pages = 0;
}

co_rc_t co_kload_begin(co_manager_t* manager, unsigned long long min_va,
		       unsigned long long max_va)
{
	unsigned long long image_lo, image_hi, need;
	co_kload_block_t* b;
	co_rc_t rc;

	co_kload_free(manager);

	if (min_va >= max_va || !CO_ARCH_VA_CANONICAL(min_va) || !CO_ARCH_VA_CANONICAL(max_va - 1))
		return CO_RC(INVALID_PARAMETER);

	if (min_va < CO_ARCH_KERNEL_MAP)
		return CO_RC(INVALID_PARAMETER);

	/*
	 * Block 0 holds the image and the page tables, and is sized to exactly
	 * that -- the one contiguous run this design still requires.
	 *
	 * The image has to be physically unbroken because __pa() is linear:
	 * x - __START_KERNEL_map + phys_base, with one phys_base for the whole
	 * kernel window. But phys_base itself is ours to choose, so the image
	 * begins at the block's base rather than 16 MB into it -- the link
	 * address's CONFIG_PHYSICAL_START offset is absorbed into phys_base
	 * instead of demanded from the allocator. That takes the contiguous
	 * requirement from image + 16 MB + tables down to image + tables.
	 *
	 * Asking for more than this here would be repeating the mistake this
	 * replaces: a fixed 128 MB demand fails on a fragmented host, and the
	 * fallback that was tried instead -- halve until something fits -- had
	 * a rung below the image's own size. What a 32 MB block produced was
	 * image frames landing on the page tables at the top of the block,
	 * reported as nothing more than a failed chunk. Any further RAM comes
	 * later, in whatever pieces the host can spare.
	 *
	 * 2 MB granularity throughout: the base because relocated PSE entries
	 * have reserved low bits, the size so the direct map can be built with
	 * 2 MB pages, and phys_base because it is added to those PSE entries.
	 */
	image_lo = min_va & ~(CO_ARCH_PMD_SIZE - 1);
	image_hi = (max_va + CO_ARCH_PMD_SIZE - 1) & ~(CO_ARCH_PMD_SIZE - 1);
	need	 = (image_hi - image_lo) + CO_KLOAD_TABLE_BYTES;

	b = kload_block_alloc(need);
	if (b == NULL) {
		co_debug_error("kload: no contiguous %lld MB for image + tables"
			       " -- that is the floor, and the host cannot supply it",
			       need >> 20);
		return CO_RC(OUT_OF_MEMORY);
	}

	kload_phys_base = b->pa - (image_lo - CO_ARCH_KERNEL_MAP);
	kload_table_top = b->bytes;

	co_debug("kload: image + tables is %lld MB at host physical 0x%llx,"
		 " phys_base 0x%llx",
		 b->bytes >> 20, (unsigned long long)b->pa, kload_phys_base);

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
	 * __START_KERNEL_map + an offset, so the page backing kernel virtual
	 * address V has to be host physical V - __START_KERNEL_map + phys_base
	 * -- that is what __pa() will compute later, and what the linear map
	 * has to agree with.
	 *
	 * The bound is the table region, not the block's end. Block 0 is sized
	 * so the image fits below the tables, so this refusal should never
	 * fire -- but when the 32 MB fallback undersized the block, image
	 * frames walked straight into the live page tables at its top, and
	 * what surfaced was not this but a later lookup returning garbage. A
	 * frame that would land in the tables is refused by name instead.
	 */
	if (page_va < CO_ARCH_KERNEL_MAP)
		return CO_RC(INVALID_PARAMETER);

	pa = kload_kva_pa(page_va);
	if (kload_block_count == 0 ||
	    pa < kload_block[0].pa ||
	    pa + CO_ARCH_PAGE_SIZE >
	    kload_block[0].pa + kload_block[0].bytes - CO_KLOAD_TABLE_BYTES) {
		co_debug_error("kload: 0x%llx implies frame 0x%llx, outside the"
			       " image block's usable range", page_va,
			       (unsigned long long)pa);
		return CO_RC(INVALID_PARAMETER);
	}
	pfn = (co_pfn_t)(pa >> CO_ARCH_PAGE_SHIFT);

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
 * Read guest memory through the guest's own page tables.
 *
 * The reader for post-mortem structure decoding: the daemon points this at the
 * printk ringbuffer's descriptors and text and gets back exactly the bytes the
 * guest would have read, because it is the same walk over the same frames. A
 * hole in the range is reported as NOT_FOUND rather than skipped -- a decoder
 * fed silently zeroed gaps produces plausible garbage, which is worse than an
 * error.
 */
co_rc_t co_kload_read(co_manager_t* manager, unsigned long long va,
		      unsigned char* buf, unsigned long size)
{
	if (kload_space == NULL)
		return CO_RC(ERROR);

	while (size) {
		unsigned long offset = (unsigned long)(va & ~CO_ARCH_PAGE_MASK);
		unsigned long part   = CO_ARCH_PAGE_SIZE - offset;
		co_pa_t pa = 0;
		int level = -1;
		unsigned char* p;

		if (part > size)
			part = size;

		if (!CO_OK(co_arch_guest_lookup(manager, kload_space, va, &pa, &level)) || !pa)
			return CO_RC(NOT_FOUND);

		p = co_kload_frame_va((co_pfn_t)(pa >> CO_ARCH_PAGE_SHIFT));
		if (p == NULL)
			return CO_RC(ERROR);

		co_memcpy(buf, p + offset, part);

		buf  += part;
		va   += part;
		size -= part;
	}

	return CO_RC(OK);
}

/*
 * Write guest memory through the guest's own page tables -- co_kload_read in
 * the other direction, and it exists for the same reason the reader does: a
 * guest virtual address is not a host pointer.
 *
 * The monitor loop is the customer. Once the guest owns its descriptor tables,
 * an interrupt lands on whatever stack the kernel was using -- the init task's
 * boot stack, a vmalloc'd thread stack -- and the frame the stub records is at
 * an address only the guest's CR3 can resolve. Dereferencing it from the host
 * was fine exactly as long as every frame lived on the passage page's IST
 * stack, and became bugcheck 0x50 the first crossing after it did not.
 */
co_rc_t co_kload_write(co_manager_t* manager, unsigned long long va,
		       const unsigned char* buf, unsigned long size)
{
	if (kload_space == NULL)
		return CO_RC(ERROR);

	while (size) {
		unsigned long offset = (unsigned long)(va & ~CO_ARCH_PAGE_MASK);
		unsigned long part   = CO_ARCH_PAGE_SIZE - offset;
		co_pa_t pa = 0;
		int level = -1;
		unsigned char* p;

		if (part > size)
			part = size;

		if (!CO_OK(co_arch_guest_lookup(manager, kload_space, va, &pa, &level)) || !pa)
			return CO_RC(NOT_FOUND);

		p = co_kload_frame_va((co_pfn_t)(pa >> CO_ARCH_PAGE_SHIFT));
		if (p == NULL)
			return CO_RC(ERROR);

		co_memcpy(p + offset, buf, part);

		buf  += part;
		va   += part;
		size -= part;
	}

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
	unsigned long long have;
	int i;

	if (kload_space == NULL || kload_block_count == 0)
		return CO_RC(ERROR);

	if (text_va < CO_ARCH_KERNEL_MAP || end_va <= text_va)
		return CO_RC(INVALID_PARAMETER);

	text_phys = kload_kva_pa(text_va) & CO_ARCH_PAGE_MASK;
	end_phys  = (kload_kva_pa(end_va) + CO_ARCH_PAGE_SIZE - 1) & CO_ARCH_PAGE_MASK;

	if (text_phys < kload_block[0].pa ||
	    end_phys > kload_block[0].pa + kload_block[0].bytes - CO_KLOAD_TABLE_BYTES) {
		co_debug_error("kload: image 0x%llx..0x%llx is not inside block 0",
			       text_phys, end_phys);
		return CO_RC(INVALID_PARAMETER);
	}

	kload_ram_bytes = ram_bytes;
	kload_text_phys = text_phys;
	kload_end_phys  = end_phys;

	/*
	 * The rest of the guest's RAM, in whatever pieces the host can spare.
	 *
	 * ram_bytes is a target, not a demand. Block 0 already exists and is
	 * counted against it; the remainder is requested in blocks no bigger
	 * than 32 MB, halving on refusal, floor 4 MB -- small enough that a
	 * host too fragmented for it has essentially nothing left. Falling
	 * short is reported and is not a failure: a guest with less memory
	 * boots further than a guest with none, and the e820 tells it exactly
	 * what it has.
	 */
	have = kload_block[0].bytes - CO_KLOAD_TABLE_BYTES;
	while (have < ram_bytes && kload_block_count < CO_KLOAD_MAX_BLOCKS) {
		unsigned long long want = ram_bytes - have;
		unsigned long long try_bytes;

		want = (want + CO_ARCH_PMD_SIZE - 1) & ~(CO_ARCH_PMD_SIZE - 1);
		if (want > (32ULL << 20))
			want = 32ULL << 20;

		for (try_bytes = want; try_bytes >= (4ULL << 20); try_bytes >>= 1) {
			if (kload_block_alloc(try_bytes))
				break;
		}
		if (try_bytes < (4ULL << 20)) {
			co_debug("kload: host has no contiguous 4 MB left,"
				 " stopping at %lld MB of %lld MB",
				 have >> 20, ram_bytes >> 20);
			break;
		}

		have += try_bytes;
	}

	co_debug("kload: %d blocks, %lld MB of RAM against a %lld MB target",
		 kload_block_count, have >> 20, ram_bytes >> 20);

	/*
	 * The direct map, over the reserved region only -- not over RAM.
	 *
	 * Mapping the whole block was the obvious thing and it is wrong, because
	 * the guest builds this map too. init_mem_mapping() walks the e820's RAM
	 * ranges and installs 2 MB pages for each; finding a host-built entry
	 * already there and disagreeing with it, set_pmd_safe warns --
	 * arch/x86/mm/init_64.c:90, which the kernel's bug table names for us --
	 * and then overwrites it anyway. Two owners of one map, and the loser is
	 * whichever ran first.
	 *
	 * Leaving the guest to own it outright was tried and is worse. On real
	 * hardware the direct map does not exist until init_mem_mapping builds
	 * it, and early faults are fixed up on demand by early_make_pgtable --
	 * but that handler lives in an IDT this guest does not use. Its CR3 is
	 * init_top_pgt, which is part of the kernel image and therefore in
	 * ordinary RAM, so the very first __va(read_cr3_pa()) needs the RAM half
	 * of this map. Removing it put the guest back five million instructions,
	 * at exactly the fault this map was added to fix.
	 *
	 * So the map stays, and the conflict has to be settled the other way:
	 * by geometry. init_mem_mapping installs 2 MB pages, this installs 4 KB
	 * ones, and set_pmd_safe warns because the entries differ in kind rather
	 * than in what they address. Building this map with 2 MB pages -- which
	 * the block's alignment already allows -- would make the kernel's write
	 * agree with what it finds. That is the next change; it needs the space
	 * builder to install a PMD with _PAGE_PSE rather than always descending
	 * to a PT.
	 */
	for (i = 0; i < kload_block_count; i++) {
		for (phys = 0; phys < kload_block[i].bytes; phys += CO_ARCH_PMD_SIZE) {
			co_pa_t pa = kload_block[i].pa + phys;
			co_rc_t rc = co_arch_guest_map_large(manager, kload_space,
							     CO_ARCH_DIRECT_MAP + pa, pa,
							     _KERNPG_TABLE | _PAGE_GLOBAL);

			if (!CO_OK(rc))
				return rc;

			kload_ram_pages += CO_ARCH_PMD_SIZE >> CO_ARCH_PAGE_SHIFT;
		}
	}

	/*
	 * The first megabyte, which on a real machine is BIOS and option ROMs.
	 *
	 * The guest's memory starts wherever the host had a contiguous block, so
	 * guest physical 0 is nothing at all -- but the kernel reads down there
	 * regardless of what the e820 says, looking for signatures: DMI, the
	 * EBDA, the video ROM. It faulted reading __va(0xc0000).
	 *
	 * One zeroed frame, mapped read-only across the whole megabyte. Reads
	 * find no signatures and the scans conclude there is nothing there,
	 * which is true. Read-only because nothing should be writing to ROM, and
	 * if something does the fault says so rather than quietly corrupting
	 * whatever else the frame is aliased to.
	 */
	{
		co_pfn_t rom;
		co_rc_t rrc = co_kload_table_frame(manager, &rom);

		if (!CO_OK(rrc))
			return rrc;

		for (phys = 0; phys < 0x100000ULL; phys += CO_ARCH_PAGE_SIZE) {
			rrc = co_arch_guest_map(manager, kload_space,
						CO_ARCH_DIRECT_MAP + phys,
						((co_pa_t)rom) << CO_ARCH_PAGE_SHIFT,
						_PAGE_PRESENT | _PAGE_ACCESSED);
			if (!CO_OK(rrc))
				return rrc;
		}
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

/*
 * Hand the guest its own page tables.
 *
 * Up to here the guest ran in an address space the host built. That is enough
 * to execute code and reach memory, and it is not enough to be Linux: the
 * kernel expects the tables it was linked with -- init_top_pgt, and the
 * level3_kernel_pgt / level2_kernel_pgt / level2_fixmap_pgt chain under it --
 * to be the live ones, and it walks and edits them directly. early_ioremap_init
 * follows that chain down to install the fixmap, finds a zero where the host's
 * space has its own arrangement instead, takes the zero as a table's physical
 * address and writes to __va(0).
 *
 * Those tables are statically initialised with link-time addresses: an entry
 * holds (target - __START_KERNEL_map), because at link time nobody knows where
 * the image will be loaded. head_64.S's __startup_64 adds the load offset to
 * every one of them before switching CR3, and this is that fixup -- the image
 * is at phys_base, so phys_base is the delta.
 *
 * Then the host's own mappings are grafted in at top level: the direct map and
 * the passage page exist only in the space the host built, and the guest cannot
 * be switched to a table that does not contain the code performing the switch.
 */
static void kload_fixup_table(co_manager_t* manager, unsigned long long table_va,
			      unsigned long long delta, int entries, const char* what)
{
	unsigned long long* t;
	co_pa_t pa;
	int i, fixed = 0;

	pa = kload_kva_pa(table_va);
	t  = (unsigned long long*)co_kload_frame_va((co_pfn_t)(pa >> CO_ARCH_PAGE_SHIFT));
	if (t == NULL) {
		co_debug_error("kload: %s is not inside the guest block", what);
		return;
	}

	for (i = 0; i < entries; i++) {
		if (!(t[i] & _PAGE_PRESENT))
			continue;
		t[i] += delta;
		fixed++;
	}

	co_debug("kload: %s, %d entries relocated by 0x%llx", what, fixed, delta);
}

co_rc_t co_kload_adopt_kernel_tables(co_manager_t* manager,
				     const unsigned long long* table_va,
				     int table_count,
				     unsigned long long* cr3_out)
{
	unsigned long long* kernel_pml4;
	unsigned long long* ours;
	co_pa_t pml4_pa;
	int i, grafted = 0;

	if (kload_space == NULL || kload_block_count == 0 || table_count < 1)
		return CO_RC(ERROR);

	/*
	 * Relocate every static table first. init_top_pgt must be table_va[0]:
	 * it is the one CR3 ends up holding, and the grafting below writes into
	 * it after it has been fixed up.
	 *
	 * The delta is phys_base: the entries hold (target - __START_KERNEL_map),
	 * which is what __startup_64 would have added the load offset to, and
	 * phys_base is exactly that offset here.
	 */
	for (i = 0; i < table_count; i++)
		kload_fixup_table(manager, table_va[i], kload_phys_base, 512,
				  i == 0 ? "init_top_pgt" : "a static kernel table");

	pml4_pa = kload_kva_pa(table_va[0]);
	kernel_pml4 = (unsigned long long*)
		co_kload_frame_va((co_pfn_t)(pml4_pa >> CO_ARCH_PAGE_SHIFT));
	if (kernel_pml4 == NULL)
		return CO_RC(ERROR);

	ours = (unsigned long long*)co_kload_frame_va(
			(co_pfn_t)(co_arch_guest_space_root(kload_space) >> CO_ARCH_PAGE_SHIFT));
	if (ours == NULL)
		return CO_RC(ERROR);

	/*
	 * The entry __startup_64 would have written.
	 *
	 * init_top_pgt is empty in the image in this kernel -- head_64.S builds
	 * it at run time, out of early_top_pgt, and that is code the host skips.
	 * The relocation pass above found nothing to relocate in it for exactly
	 * that reason: zero entries present. So the one that matters is written
	 * here, the kernel half of the address space, pointing at
	 * level3_kernel_pgt, which does have its static contents and has just
	 * been relocated.
	 *
	 * Without it CR3 held a blank top-level table with only the host's
	 * grafted entries in it, and the first walk down to the fixmap found a
	 * zero, took it for a table's physical address and wrote to __va(0).
	 */
	if (table_count > 1 &&
	    !(kernel_pml4[CO_ARCH_PGD_INDEX(CO_ARCH_KERNEL_MAP)] & _PAGE_PRESENT)) {
		co_pa_t l3 = kload_kva_pa(table_va[1]);

		kernel_pml4[CO_ARCH_PGD_INDEX(CO_ARCH_KERNEL_MAP)] = l3 | _KERNPG_TABLE;
		co_debug("kload: wired init_top_pgt[%d] -> level3_kernel_pgt at 0x%llx",
			 (int)CO_ARCH_PGD_INDEX(CO_ARCH_KERNEL_MAP),
			 (unsigned long long)l3);
	}

	/*
	 * Anything the host mapped and the kernel's table has no opinion about.
	 * That is the direct map and the passage page; the kernel's own entry
	 * for its image is left exactly as linked, which is the entire point.
	 */
	for (i = 0; i < 512; i++) {
		if (!(ours[i] & _PAGE_PRESENT))
			continue;
		if (kernel_pml4[i] & _PAGE_PRESENT)
			continue;

		kernel_pml4[i] = ours[i];
		grafted++;
	}

	co_debug("kload: adopted the kernel's tables, cr3 0x%llx, %d host entries grafted",
		 (unsigned long long)pml4_pa, grafted);

	*cr3_out = pml4_pa;
	return CO_RC(OK);
}

unsigned long co_kload_ram_pages(void)
{
	return kload_ram_pages;
}
