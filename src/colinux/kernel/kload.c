/*
 * This source code is a part of coLinux source package.
 *
 * The code is licensed under the GPL. See the COPYING file at
 * the root directory.
 */

/*
 * Load a kernel into dense pseudo-physical RAM backed by scattered host pages.
 *
 * The image is visible both at its linked addresses and through Linux's direct
 * map. Host-built page tables contain machine PFNs; Linux-facing e820, device
 * requests and page-table values contain pseudo PFNs. The p2m/m2p pair below is
 * the boundary between those two address spaces.
 *
 * One guest exists at a time, held in file-static state. The boot daemon
 * enforces that at the process boundary and kload_lock serializes teardown.
 */

/* GCC's own freestanding stddef.h, for size_t as a pointer-sized integer. */
#include <stddef.h>

#include <colinux/common/common.h>
#include <colinux/common/debug.h>
#include <colinux/common/libc.h>
#include <colinux/common/ioctl.h>
#include <colinux/os/kernel/alloc.h>
#include <colinux/os/kernel/misc.h>
#include <colinux/os/kernel/mutex.h>
#include <colinux/os/timer.h>		/* co_os_msleep, for the deferred-free wait */
#include <colinux/arch/mmu.h>
#include <colinux/arch/space.h>

#include "manager.h"
#include "kload.h"
#include "console.h"
#include "net.h"
#include "vgpu.h"

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
 * Linux sees a dense pseudo-physical address space beginning at zero.  Host
 * memory is obtained from cached nonpaged pool, which gives each allocation a
 * contiguous kernel virtual address but places its individual pages wherever
 * free machine frames exist.  No allocation below asks Windows for physically
 * contiguous memory.
 *
 * p2m[pseudo PFN] is the machine PFN written into a hardware page-table entry.
 * m2p is an open-addressed reverse hash whose slot contains pseudo PFN + 1;
 * zero is therefore an empty slot.  The reverse map keeps page-table walks and
 * host I/O off a scan of hundreds of thousands of entries.
 */
typedef struct {
	void*		   va;
	unsigned long long pseudo_base;
	unsigned long long bytes;
} co_kload_block_t;

static co_kload_block_t	      kload_block[CO_KLOAD_MAX_BLOCKS];
static int		      kload_block_count;
static int		      kload_last_hit;

static co_pfn_t*	      kload_p2m;
static unsigned int*	      kload_m2p;
static unsigned int	      kload_p2m_alloc_pages;
static unsigned int	      kload_m2p_alloc_pages;
static unsigned long	      kload_p2m_capacity;
static unsigned long	      kload_backed_pages;
static unsigned long	      kload_m2p_slots;
static unsigned long	      kload_m2p_mask_value;

/*
 * Teardown is serialised, because three unrelated threads can ask for it.
 *
 * co_kload_free has always been reachable from the KLOAD_END ioctl, from
 * co_kload_begin at the start of a new run, and from the manager's unload path
 * when the last handle closes. Its only guard was the early return below, which
 * two threads can both fall through before either has cleared anything: they
 * then both walk kload_block[] and hand the same pointer to
 * MmFreeContiguousMemory twice. The second call is bugcheck 0xC2 with parameter
 * one 0x60 -- "invalid contiguous memory address" -- which is what a
 * successfully freed address looks like the second time.
 *
 * It went unhit for as long as it did because the window is the length of the
 * free loop. At sixteen blocks that was sixteen frees; a 1 GB guest in 8 MB
 * pieces makes it a hundred and twenty four, and it took one evening.
 *
 * Idempotence rather than exclusion is the requirement: every one of those
 * callers is entitled to ask, and all but the first must find nothing to do.
 */
static co_os_mutex_t	      kload_lock;
static unsigned long long     kload_phys_base;	/* what the guest's __pa() adds */
static unsigned long long     kload_table_start;
static unsigned long long     kload_table_end;
static unsigned long long     kload_table_top;	/* pseudo PA; tables grow down */

/*
 * User-mode mappings of the blocks, and why teardown has to wait for them.
 *
 * R3 hands a host process a direct window onto guest RAM (see KMAP in
 * ioctl.h). Those mappings name physical pages inside kload_block[], so
 * freeing a block while one is live gives a user process a writable window
 * onto memory the host has re-issued to something else. That is not a crash,
 * which is what makes it worse than one: it is silent corruption of an
 * unrelated driver's data, discovered later and somewhere else.
 *
 * So teardown is split. co_kload_free still does everything that stops the
 * guest -- retires the cross-process readers, publishes kload_space = NULL,
 * destroys the address space -- because none of that is what the mappings
 * name. But if a mapping is outstanding it sets pending_free and returns
 * WITHOUT releasing the pages; the last unmap runs the release instead.
 *
 * The count is only ever touched under kload_lock. The waiter in
 * co_kload_begin deliberately does NOT hold the lock while it waits, because
 * the handle-close that would satisfy it needs the same lock -- holding it
 * across the wait is a guaranteed ten-second livelock followed by a refused
 * boot, which is a worse failure than the one being prevented.
 */
static int		      kload_user_maps;
static bool_t		      kload_pending_free;
/* KMAP may take a snapshot only after KRAM has finished growing the blocks. */
static bool_t		      kload_ram_ready;

#define CO_KLOAD_MIN_TABLE_BYTES (4ULL << 20)

/*
 * Address space above guest RAM in which host memory windows may appear.
 *
 * Sizing capacity to exactly RAM + tables gave the downward window allocator
 * zero pages to hand out: kload_window_next started level with the top of
 * backed RAM and the first KWINDOW met the allocators-meet check immediately.
 * The arena is address space, not memory -- its p2m slots stay zero until a
 * window fills them, nothing appears in the guest's e820, and the only real
 * cost is metadata: 4 GB of arena is 8 MB of p2m slots (plus the m2p hash
 * sized from capacity, which windows deliberately never populate).
 */
#define CO_KLOAD_WINDOW_ARENA_BYTES (4ULL << 30)

static unsigned long kload_m2p_hash(co_pfn_t mfn)
{
	return (unsigned long)((mfn * 11400714819323198485ULL)
			       & (co_pfn_t)kload_m2p_mask_value);
}

static bool_t kload_machine_to_pseudo_pfn(co_pfn_t mfn, co_pfn_t* ppfn_out)
{
	unsigned long slot, probes;

	if (kload_m2p == NULL || kload_m2p_slots == 0)
		return PFALSE;

	slot = kload_m2p_hash(mfn);
	for (probes = 0; probes < kload_m2p_slots; probes++) {
		unsigned int stored = kload_m2p[slot];
		co_pfn_t ppfn;

		if (stored == 0)
			return PFALSE;

		ppfn = (co_pfn_t)(stored - 1);
		if (ppfn < kload_backed_pages && kload_p2m[ppfn] == mfn) {
			*ppfn_out = ppfn;
			return PTRUE;
		}

		slot = (slot + 1) & kload_m2p_mask_value;
	}

	return PFALSE;
}

static bool_t kload_pseudo_to_machine_pfn(co_pfn_t ppfn, co_pfn_t* mfn_out)
{
	if (kload_p2m == NULL || ppfn >= kload_p2m_capacity)
		return PFALSE;

	/*
	 * Pages above backed RAM are host windows (see the window allocator
	 * below), and only the ones actually mapped may translate. The array is
	 * zeroed at allocation and machine frame zero is never a guest page, so
	 * a zero entry is exactly "nothing is mapped here" -- which is the same
	 * answer this used to give for every address above guest RAM, only now
	 * it is a fact about the entry rather than about the boundary.
	 */
	if (ppfn >= kload_backed_pages && kload_p2m[ppfn] == 0)
		return PFALSE;

	*mfn_out = kload_p2m[ppfn];
	return PTRUE;
}

/*
 * Host memory windows in the guest's address space.
 *
 * Vulkan's contract is that the application writes into memory the GPU reads,
 * so a guest running Venus needs host GPU-visible memory to appear in its own
 * physical address space. The p2m already performs exactly that translation
 * for guest RAM; a window is the same mechanism pointed at frames the host
 * obtained from somewhere else.
 *
 * Nothing is reserved. QEMU's Windows Venus port pins a fixed multi-gigabyte
 * region up front because WHPX makes it establish guest-physical ranges in
 * advance, whether or not anything ever uses them. We own the p2m, so we can
 * do what the rest of this file already does with guest RAM: take what the
 * host can give, at the moment something asks for it, and give it back on
 * release. A refusal is an ordinary outcome and the caller is expected to
 * cope -- the same contract as kload_block_alloc returning NULL.
 *
 * Windows are allocated downward from the top of p2m capacity while guest RAM
 * grows upward from zero, so neither has to predict how much the other will
 * want; they simply must not meet.
 *
 * m2p is deliberately NOT populated. The reverse map answers "which guest page
 * is this machine frame", and these frames are not guest pages -- they belong
 * to a host allocation that merely appears in the guest's address space. A
 * reverse entry would let a page-table walk mistake GPU memory for guest RAM.
 */
static unsigned long kload_window_next;	/* lowest window PFN handed out */
/* First PFN above the RAM ceiling; the arena is [this, capacity). Pinned in
 * kload_begin -- see the comment there for why backed_pages must not be it. */
static unsigned long kload_window_base_pages;

co_rc_t co_kload_window_map(const co_pfn_t* mfns, unsigned long count,
			    co_pa_t* pseudo_out)
{
	unsigned long first, i;

	if (mfns == NULL || pseudo_out == NULL || count == 0)
		return CO_RC(INVALID_PARAMETER);

	co_os_mutex_acquire(kload_lock);

	if (kload_p2m == NULL) {
		co_os_mutex_release(kload_lock);
		return CO_RC(ERROR);
	}

	/* The two allocators meet in the middle; whoever asks second loses. */
	if (kload_window_next < count ||
	    kload_window_next - count < kload_window_base_pages) {
		co_os_mutex_release(kload_lock);
		return CO_RC(OUT_OF_MEMORY);
	}

	first = kload_window_next - count;
	for (i = 0; i < count; i++) {
		/*
		 * Frame zero would be indistinguishable from an unmapped entry
		 * and would silently punch a hole in the middle of a window.
		 */
		if (mfns[i] == 0) {
			while (i-- > 0)
				kload_p2m[first + i] = 0;
			co_os_mutex_release(kload_lock);
			return CO_RC(INVALID_PARAMETER);
		}
		kload_p2m[first + i] = mfns[i];
	}

	kload_window_next = first;
	*pseudo_out = ((co_pa_t)first) << CO_ARCH_PAGE_SHIFT;

	co_os_mutex_release(kload_lock);
	return CO_RC(OK);
}

/*
 * A window at an address the CALLER chose. RESOURCE_MAP_BLOB works this way
 * round: the guest's drm_mm picks the offset inside the advertised region and
 * the host is told, not asked. So this side validates rather than allocates --
 * the range must lie inside the arena and every slot must be vacant. The
 * downward allocator above and the guest's bottom-up drm_mm share the arena
 * without coordination; whoever asks for an occupied slot is refused, which
 * both sides treat as an ordinary answer.
 */
co_rc_t co_kload_window_map_at(const co_pfn_t* mfns, unsigned long count,
			       co_pa_t pseudo)
{
	unsigned long first = (unsigned long)(pseudo >> CO_ARCH_PAGE_SHIFT);
	unsigned long i;

	if (mfns == NULL || count == 0 ||
	    (pseudo & (CO_ARCH_PAGE_SIZE - 1)) != 0)
		return CO_RC(INVALID_PARAMETER);

	co_os_mutex_acquire(kload_lock);

	if (kload_p2m == NULL) {
		co_os_mutex_release(kload_lock);
		return CO_RC(ERROR);
	}

	if (first < kload_window_base_pages ||
	    count > kload_p2m_capacity - first) {
		co_os_mutex_release(kload_lock);
		return CO_RC(INVALID_PARAMETER);
	}

	for (i = 0; i < count; i++) {
		if (kload_p2m[first + i] != 0) {
			co_os_mutex_release(kload_lock);
			return CO_RC(OUT_OF_MEMORY);
		}
	}

	for (i = 0; i < count; i++) {
		if (mfns[i] == 0) {
			while (i-- > 0)
				kload_p2m[first + i] = 0;
			co_os_mutex_release(kload_lock);
			return CO_RC(INVALID_PARAMETER);
		}
		kload_p2m[first + i] = mfns[i];
	}

	co_os_mutex_release(kload_lock);
	return CO_RC(OK);
}

void co_kload_window_unmap(co_pa_t pseudo, unsigned long count)
{
	unsigned long first = (unsigned long)(pseudo >> CO_ARCH_PAGE_SHIFT);
	unsigned long i;

	if (count == 0)
		return;

	co_os_mutex_acquire(kload_lock);

	/*
	 * No watermark refusal here: caller-placed windows (map_at) live below
	 * kload_window_next by design. What userspace may free is already
	 * gated by the per-handle exact match in co_manager_window_release;
	 * this check is only against corrupting RAM's entries.
	 */
	if (kload_p2m == NULL || first < kload_window_base_pages ||
	    count > kload_p2m_capacity - first) {
		co_os_mutex_release(kload_lock);
		return;
	}

	for (i = 0; i < count; i++)
		kload_p2m[first + i] = 0;

	/*
	 * Address space comes back only when the most recent window is the one
	 * released; anything else leaves a hole that later windows skip over.
	 * Windows are few and long-lived, so a free list would be bookkeeping
	 * for a case that does not arise -- and the entries are cleared either
	 * way, which is what stops the guest from reaching the memory.
	 */
	if (first == kload_window_next)
		kload_window_next += count;

	co_os_mutex_release(kload_lock);
}

static co_pa_t kload_pseudo_to_machine_pa(co_pa_t pseudo)
{
	co_pfn_t mfn;

	if (!kload_pseudo_to_machine_pfn(
			(co_pfn_t)(pseudo >> CO_ARCH_PAGE_SHIFT), &mfn))
		return 0;

	return (((co_pa_t)mfn) << CO_ARCH_PAGE_SHIFT)
		| (pseudo & ~CO_ARCH_PAGE_MASK);
}

/*
 * Translate a Linux-visible pseudo-physical address for host consumers.
 *
 * Most of the p2m boundary is page-table construction, where the translation
 * stays private to this file.  A START_VCPU request is the exception: Linux
 * names init_mm.pgd with __pa(), so the CR3 in the request is pseudo-physical,
 * while the switch must load and compare the machine CR3.  Keeping this small
 * checked interface avoids exposing the p2m array itself to the SMP path.
 */
co_rc_t co_kload_pseudo_to_machine(co_pa_t pseudo, co_pa_t* machine)
{
	co_pfn_t mfn;

	if (machine == NULL ||
	    !kload_pseudo_to_machine_pfn(
		(co_pfn_t)(pseudo >> CO_ARCH_PAGE_SHIFT), &mfn))
		return CO_RC(NOT_FOUND);

	*machine = (((co_pa_t)mfn) << CO_ARCH_PAGE_SHIFT)
		| (pseudo & ~CO_ARCH_PAGE_MASK);
	return CO_RC(OK);
}

/* Resolve a pseudo address through the virtually contiguous block index. */
void* co_kload_pseudo_frame_va(co_pfn_t pfn)
{
	unsigned long long pa = ((unsigned long long)pfn) << CO_ARCH_PAGE_SHIFT;
	int lo, hi, last;

	if (pfn >= kload_backed_pages)
		return NULL;

	last = kload_last_hit;
	if (last < kload_block_count && kload_block[last].va != NULL &&
	    pa >= kload_block[last].pseudo_base &&
	    pa < kload_block[last].pseudo_base + kload_block[last].bytes)
		return (void*)((char*)kload_block[last].va
			       + (unsigned long)(pa - kload_block[last].pseudo_base));

	lo = 0;
	hi = kload_block_count - 1;
	while (lo <= hi) {
		int mid = lo + (hi - lo) / 2;
		co_kload_block_t* b = &kload_block[mid];

		if (pa < b->pseudo_base) {
			hi = mid - 1;
		} else if (pa >= b->pseudo_base + b->bytes) {
			lo = mid + 1;
		} else {
			kload_last_hit = mid;
			return (void*)((char*)b->va
				       + (unsigned long)(pa - b->pseudo_base));
		}
	}

	return NULL;
}

/* Hardware page-table entries contain machine PFNs. */
void* co_kload_frame_va(co_pfn_t pfn)
{
	co_pfn_t ppfn;

	if (!kload_machine_to_pseudo_pfn(pfn, &ppfn))
		return NULL;
	return co_kload_pseudo_frame_va(ppfn);
}

unsigned long long co_kload_phys_base(void)
{
	return kload_phys_base;
}

int co_kload_range_count(void)
{
	if (kload_block_count == 0)
		return 0;
	return ((((unsigned long long)kload_backed_pages) << CO_ARCH_PAGE_SHIFT)
		> kload_table_end) ? 2 : 1;
}

/*
 * One block as the guest should hear about it: a usable range, and for block 0
 * the reserved page-table region above it. The e820 is built from exactly this.
 */
void co_kload_range(int i, unsigned long long* pa, unsigned long long* usable,
		    unsigned long long* reserved)
{
	*pa = *usable = *reserved = 0;

	if (i < 0 || i >= co_kload_range_count())
		return;

	if (i == 0) {
		*pa	  = 0;
		*usable	  = kload_table_start;
		*reserved = kload_table_end - kload_table_start;
	} else {
		*pa	  = kload_table_end;
		*usable	  = (((unsigned long long)kload_backed_pages)
			     << CO_ARCH_PAGE_SHIFT) - kload_table_end;
	}
}

/* The pseudo-physical address the guest's own __pa() computes for kva. */
static co_pa_t kload_kva_pa(unsigned long long kva)
{
	return (co_pa_t)(kva - CO_ARCH_KERNEL_MAP + kload_phys_base);
}

static co_kload_block_t* kload_block_alloc(unsigned long long bytes)
{
	co_kload_block_t* b;
	void* va;
	unsigned long pages, i;
	unsigned long first;

	if (kload_block_count >= CO_KLOAD_MAX_BLOCKS)
		return NULL;

	pages = (unsigned long)(bytes >> CO_ARCH_PAGE_SHIFT);
	if (pages == 0 || pages > kload_p2m_capacity - kload_backed_pages)
		return NULL;

	va = co_os_alloc_cached_pages((unsigned int)pages);
	if (va == NULL)
		return NULL;

	co_memset(va, 0, (long)(pages << CO_ARCH_PAGE_SHIFT));
	first = kload_backed_pages;
	for (i = 0; i < pages; i++) {
		co_pa_t machine = co_os_virt_to_phys(
			(void*)((char*)va + (i << CO_ARCH_PAGE_SHIFT)));
		co_pfn_t mfn = (co_pfn_t)(machine >> CO_ARCH_PAGE_SHIFT);
		unsigned long slot = kload_m2p_hash(mfn);

		kload_p2m[first + i] = mfn;
		while (kload_m2p[slot] != 0)
			slot = (slot + 1) & kload_m2p_mask_value;
		kload_m2p[slot] = (unsigned int)(first + i + 1);
	}

	b = &kload_block[kload_block_count++];
	b->va		= va;
	b->pseudo_base = ((unsigned long long)first) << CO_ARCH_PAGE_SHIFT;
	b->bytes	= ((unsigned long long)pages) << CO_ARCH_PAGE_SHIFT;
	kload_backed_pages += pages;

	return b;
}

static bool_t kload_grow_to(unsigned long long end)
{
	while ((((unsigned long long)kload_backed_pages) << CO_ARCH_PAGE_SHIFT) < end) {
		unsigned long long have = ((unsigned long long)kload_backed_pages)
					  << CO_ARCH_PAGE_SHIFT;
		unsigned long long want = end - have;
		unsigned long long attempt;

		if (want > CO_KLOAD_CHUNK_BYTES)
			want = CO_KLOAD_CHUNK_BYTES;
		want &= CO_ARCH_PAGE_MASK;
		if (want == 0)
			want = CO_ARCH_PAGE_SIZE;

		attempt = want;
		for (;;) {
			if (kload_block_alloc(attempt) != NULL)
				break;
			if (attempt == CO_ARCH_PAGE_SIZE)
				return PFALSE;
			attempt = (attempt >> 1) & CO_ARCH_PAGE_MASK;
			if (attempt < CO_ARCH_PAGE_SIZE)
				attempt = CO_ARCH_PAGE_SIZE;
		}
	}

	return PTRUE;
}

/* A machine frame taken from the pseudo-physical reserved table range. */
co_rc_t co_kload_table_frame(co_manager_t* manager, co_pfn_t* pfn_out)
{
	unsigned long long* table;
	co_pfn_t ppfn, mfn;

	if (kload_block_count == 0)
		return CO_RC(ERROR);

	if (kload_table_top < kload_table_start + CO_ARCH_PAGE_SIZE) {
		co_debug_error("kload: out of reserved pseudo-physical page-table space");
		return CO_RC(OUT_OF_MEMORY);
	}

	kload_table_top -= CO_ARCH_PAGE_SIZE;
	ppfn = (co_pfn_t)(kload_table_top >> CO_ARCH_PAGE_SHIFT);
	if (!kload_pseudo_to_machine_pfn(ppfn, &mfn))
		return CO_RC(ERROR);

	table = co_kload_pseudo_frame_va(ppfn);
	if (table == NULL)
		return CO_RC(ERROR);
	co_memset(table, 0, CO_ARCH_PAGE_SIZE);

	*pfn_out = mfn;
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
	int count = kload_block_count;

	/*
	 * The count goes to zero before the first free, not after the last.
	 *
	 * Between reading it and clearing it this loop hands a set of pool
	 * allocations back to Windows, and anything that
	 * reaches co_kload_free in that window used to see a non-zero count and
	 * free them all a second time. The lock in co_kload_free is what
	 * actually closes that, but a torn-down block array should not be
	 * re-freeable even if the lock is ever lost again, so the loop works
	 * from a local count and leaves the global at zero throughout.
	 */
	kload_block_count = 0;
	/*
	 * Before the frees, so no lookup racing this can be handed an index
	 * into a block that is on its way out.
	 */
	kload_last_hit	  = 0;

	for (i = 0; i < count; i++) {
		void*		   va    = kload_block[i].va;
		unsigned long long bytes = kload_block[i].bytes;

		if (va == NULL)
			continue;

		/*
		 * Clear the slot before the free, for the same reason.
		 *
		 * The pool free is not instantaneous and this pointer
		 * must not be visible to anything after it has been handed over.
		 */
		kload_block[i].va	  = NULL;
		kload_block[i].pseudo_base = 0;
		kload_block[i].bytes	  = 0;

		co_os_free_cached_pages(va,
					(unsigned int)(bytes >> CO_ARCH_PAGE_SHIFT));
	}

	if (kload_p2m != NULL)
		co_os_free_cached_pages(kload_p2m, kload_p2m_alloc_pages);
	if (kload_m2p != NULL)
		co_os_free_cached_pages(kload_m2p, kload_m2p_alloc_pages);

	kload_p2m		= NULL;
	kload_m2p		= NULL;
	kload_p2m_alloc_pages = 0;
	kload_m2p_alloc_pages = 0;
	kload_p2m_capacity	= 0;
	kload_backed_pages	= 0;
	kload_window_next	= 0;
	kload_window_base_pages = 0;
	kload_m2p_slots	= 0;
	kload_m2p_mask_value	= 0;
	kload_ram_ready	= PFALSE;

	kload_phys_base	  = 0;
	kload_table_start = 0;
	kload_table_end   = 0;
	kload_table_top	  = 0;
}

void co_kload_free(co_manager_t* manager)
{
	co_arch_guest_space_t* space;

	co_os_mutex_acquire(kload_lock);
	kload_ram_ready = PFALSE;

	space = kload_space;

	if (space == NULL && kload_block_count == 0 &&
	    kload_p2m == NULL && kload_m2p == NULL) {
		co_os_mutex_release(kload_lock);
		return;
	}

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
	co_vgpu_set_address(manager, 0);

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

	/*
	 * The pages, unless a user-mode mapping still names them.
	 *
	 * Everything above has already happened: the guest is stopped, the
	 * readers are retired and the address space is gone. What is deferred
	 * is only the physical release, and only while a KMAP is outstanding.
	 * co_kload_user_map_put runs it when the last one goes away -- which
	 * happens at process exit even if the process never asked, because
	 * IRP_MJ_CLEANUP unmaps what the descriptor recorded.
	 */
	if (kload_user_maps != 0) {
		kload_pending_free = PTRUE;
		co_debug("kload: teardown deferred, %d user mapping(s) outstanding",
			 kload_user_maps);
		co_os_mutex_release(kload_lock);
		return;
	}

	kload_release_pages(manager);

	kload_pages     = 0;
	kload_chunks    = 0;
	kload_ram_bytes = 0;
	kload_ram_pages = 0;

	co_os_mutex_release(kload_lock);
}

/*
 * Reserve the completed block set for a user mapping.
 *
 * The GPU daemon starts beside the boot daemon. KLOAD_BEGIN first backs only
 * enough of the image to load it, then KRAM grows the same block array to the
 * requested RAM size. The old unconditional reference could land between
 * those stages and produce a perfectly valid mapping of the blocks that
 * existed at that instant -- 635 MB in the observed failure -- which silently
 * stayed a prefix after KRAM finished the 1 GB guest. CoXWire and virtio-gpu
 * then rejected every later page as "outside guest RAM".
 *
 * Readiness and the lifetime reference are one locked operation. This also
 * closes the teardown race in which KLOAD_END could release a block after
 * KMAP had read it but before the old post-mapping reference was taken.
 */
bool_t co_kload_user_map_try_get(void)
{
	bool_t ready;

	co_os_mutex_acquire(kload_lock);
	ready = kload_ram_ready && !kload_pending_free &&
		kload_block_count != 0;
	if (ready)
		kload_user_maps++;
	co_os_mutex_release(kload_lock);

	return ready;
}

/*
 * A user mapping went away. Takes kload_lock itself, so it is safe to call
 * from a close path that holds nothing.
 *
 * This is where a deferred teardown finally completes. It is deliberately the
 * unmapper's job rather than a watchdog's: the process that held the window is
 * the only one that knows it has let go, and by the time this returns the
 * pages are either released or still named by somebody else.
 */
void co_kload_user_map_put(co_manager_t* manager)
{
	co_os_mutex_acquire(kload_lock);

	if (kload_user_maps > 0)
		kload_user_maps--;

	if (kload_user_maps == 0 && kload_pending_free) {
		co_debug("kload: last user mapping gone, completing deferred teardown");
		kload_release_pages(manager);
		kload_pages	   = 0;
		kload_chunks	   = 0;
		kload_ram_bytes	   = 0;
		kload_ram_pages	   = 0;
		kload_pending_free = PFALSE;
	}

	co_os_mutex_release(kload_lock);
}

/*
 * Blocks, for the mapper. Under the lock, because kload_release_pages clears
 * these slots one at a time while it frees.
 */
int co_kload_block_count(void)
{
	int n;

	co_os_mutex_acquire(kload_lock);
	n = kload_block_count;
	co_os_mutex_release(kload_lock);
	return n;
}

co_rc_t co_kload_block(int i, void** va_out, unsigned long long* pa_out,
		       unsigned long long* bytes_out)
{
	co_rc_t rc = CO_RC(ERROR);

	co_os_mutex_acquire(kload_lock);

	if (i >= 0 && i < kload_block_count && kload_block[i].va != NULL) {
		*va_out	   = kload_block[i].va;
		*pa_out	   = kload_block[i].pseudo_base;
		*bytes_out = kload_block[i].bytes;
		rc	   = CO_RC(OK);
	}

	co_os_mutex_release(kload_lock);
	return rc;
}

/*
 * Find the allocation block containing a pseudo-physical address.
 *
 * KMAP_RANGE uses the block base to choose a deterministic 12 MB slice. Doing
 * that in the driver, rather than independently in every userspace caller,
 * means two requests for the same page always name the same mapping and never
 * consume duplicate VADs. The blocks are appended in pseudo-address order, so
 * a binary search stays cheap even at the 128 GB block-table ceiling.
 */
co_rc_t co_kload_block_for_pa(unsigned long long query_pa, void** va_out,
			      unsigned long long* pa_out,
			      unsigned long long* bytes_out)
{
	co_rc_t rc = CO_RC(ERROR);
	int lo, hi;

	if (!va_out || !pa_out || !bytes_out)
		return CO_RC(INVALID_PARAMETER);

	co_os_mutex_acquire(kload_lock);
	lo = 0;
	hi = kload_block_count - 1;
	while (lo <= hi) {
		int mid = lo + (hi - lo) / 2;
		co_kload_block_t* block = &kload_block[mid];

		if (query_pa < block->pseudo_base) {
			hi = mid - 1;
		} else if (query_pa >= block->pseudo_base + block->bytes) {
			lo = mid + 1;
		} else if (block->va != NULL) {
			*va_out = block->va;
			*pa_out = block->pseudo_base;
			*bytes_out = block->bytes;
			rc = CO_RC(OK);
			break;
		} else {
			break;
		}
	}
	co_os_mutex_release(kload_lock);
	return rc;
}

/*
 * Whether a new run may start yet.
 *
 * Waits outside the lock, in short sleeps, for a deferred teardown to finish.
 * Ten seconds and then a loud refusal: the alternative -- proceeding anyway --
 * would re-allocate guest RAM while a user process still has a window onto the
 * previous run's pages.
 */
static bool_t kload_wait_for_deferred_free(void)
{
	int waited;

	for (waited = 0; waited < 10000; waited += 50) {
		bool_t pending;

		co_os_mutex_acquire(kload_lock);
		pending = kload_pending_free;
		co_os_mutex_release(kload_lock);

		if (!pending)
			return PTRUE;

		co_os_msleep(50);
	}

	co_debug("kload: a user mapping has held guest RAM for 10s; refusing to start");
	return PFALSE;
}

co_rc_t co_kload_init(void)
{
	return co_os_mutex_create(&kload_lock);
}

void co_kload_fini(void)
{
	if (kload_lock != NULL) {
		co_os_mutex_destroy(kload_lock);
		kload_lock = NULL;
	}
}

static co_rc_t kload_translation_alloc(unsigned long capacity_pages)
{
	unsigned long long p2m_bytes;
	unsigned long long m2p_bytes;
	unsigned long long wanted_slots;
	unsigned long long alloc_pages;

	if (capacity_pages == 0)
		return CO_RC(INVALID_PARAMETER);

	/*
	 * A power-of-two table at least 1.5x the population stays below a 2/3
	 * load factor. The former 2x request needlessly crossed the next power of
	 * two at 128 GB because the reserved page-table pages put capacity just
	 * above 2^25: that made m2p 512 MB instead of 256 MB for no useful gain.
	 */
	wanted_slots = (unsigned long long)capacity_pages +
			 ((unsigned long long)capacity_pages + 1) / 2;
	if (wanted_slots > (unsigned long long)~0UL)
		return CO_RC(INVALID_PARAMETER);

	kload_m2p_slots = 1;
	while ((unsigned long long)kload_m2p_slots < wanted_slots) {
		if (kload_m2p_slots > (~0UL >> 1))
			return CO_RC(INVALID_PARAMETER);
		kload_m2p_slots <<= 1;
	}
	kload_m2p_mask_value = kload_m2p_slots - 1;

	p2m_bytes = (unsigned long long)capacity_pages * sizeof(kload_p2m[0]);
	m2p_bytes = (unsigned long long)kload_m2p_slots * sizeof(kload_m2p[0]);

	/* The guest reserves adjacent 1 GB virtual windows for these mappings. */
	if (p2m_bytes > CO_KLOAD_TRANSLATION_WINDOW_BYTES ||
	    m2p_bytes > CO_KLOAD_TRANSLATION_WINDOW_BYTES) {
		kload_m2p_slots = 0;
		kload_m2p_mask_value = 0;
		return CO_RC(INVALID_PARAMETER);
	}

	alloc_pages = (p2m_bytes + CO_ARCH_PAGE_SIZE - 1) >> CO_ARCH_PAGE_SHIFT;
	if (alloc_pages > (unsigned long long)~0U)
		return CO_RC(INVALID_PARAMETER);
	kload_p2m_alloc_pages = (unsigned int)alloc_pages;
	alloc_pages = (m2p_bytes + CO_ARCH_PAGE_SIZE - 1) >> CO_ARCH_PAGE_SHIFT;
	if (alloc_pages > (unsigned long long)~0U)
		return CO_RC(INVALID_PARAMETER);
	kload_m2p_alloc_pages = (unsigned int)alloc_pages;

	co_debug("kload: translation metadata p2m %llu MB, m2p %llu MB"
		 " (%lu slots)", p2m_bytes >> 20, m2p_bytes >> 20,
		 kload_m2p_slots);

	kload_p2m = (co_pfn_t*)co_os_alloc_cached_pages(kload_p2m_alloc_pages);
	if (kload_p2m == NULL) {
		kload_m2p_slots = 0;
		kload_m2p_mask_value = 0;
		return CO_RC(OUT_OF_MEMORY);
	}

	kload_m2p = (unsigned int*)co_os_alloc_cached_pages(kload_m2p_alloc_pages);
	if (kload_m2p == NULL) {
		co_os_free_cached_pages(kload_p2m, kload_p2m_alloc_pages);
		kload_p2m = NULL;
		kload_p2m_alloc_pages = 0;
		kload_m2p_slots = 0;
		kload_m2p_mask_value = 0;
		return CO_RC(OUT_OF_MEMORY);
	}

	co_memset(kload_p2m, 0,
		  (long)(kload_p2m_alloc_pages << CO_ARCH_PAGE_SHIFT));
	co_memset(kload_m2p, 0,
		  (long)(kload_m2p_alloc_pages << CO_ARCH_PAGE_SHIFT));
	kload_p2m_capacity = capacity_pages;
	/* Windows grow down from the top; guest RAM grows up from zero. */
	kload_window_next = capacity_pages;
	return CO_RC(OK);
}

static co_rc_t kload_map_translation_pages(co_manager_t* manager)
{
	unsigned int i;

	for (i = 0; i < kload_p2m_alloc_pages; i++) {
		void* va = (void*)((char*)kload_p2m + (i << CO_ARCH_PAGE_SHIFT));
		co_rc_t rc = co_arch_guest_map(manager, kload_space,
			CO_KLOAD_P2M_VA + (((unsigned long long)i) << CO_ARCH_PAGE_SHIFT),
			co_os_virt_to_phys(va),
			_KERNPG_TABLE | _PAGE_GLOBAL | CO_ARCH_PAGE_NX);

		if (!CO_OK(rc))
			return rc;
	}

	for (i = 0; i < kload_m2p_alloc_pages; i++) {
		void* va = (void*)((char*)kload_m2p + (i << CO_ARCH_PAGE_SHIFT));
		co_rc_t rc = co_arch_guest_map(manager, kload_space,
			CO_KLOAD_M2P_VA + (((unsigned long long)i) << CO_ARCH_PAGE_SHIFT),
			co_os_virt_to_phys(va),
			_KERNPG_TABLE | _PAGE_GLOBAL | CO_ARCH_PAGE_NX);

		if (!CO_OK(rc))
			return rc;
	}

	return CO_RC(OK);
}

co_rc_t co_kload_begin(co_manager_t* manager, unsigned long long min_va,
		       unsigned long long max_va, unsigned long long ram_bytes)
{
	unsigned long long image_hi, table_bytes, capacity_end, sizing_bytes;
	co_rc_t rc;

	co_kload_free(manager);

	/*
	 * If the previous run's pages are still named by a user mapping, the
	 * free above only deferred. Starting anyway would allocate this run's
	 * RAM while another process holds a window onto the last run's -- so
	 * wait for the unmap, outside the lock, and refuse loudly on timeout.
	 */
	if (!kload_wait_for_deferred_free())
		return CO_RC(ERROR);

	if (min_va >= max_va || !CO_ARCH_VA_CANONICAL(min_va) || !CO_ARCH_VA_CANONICAL(max_va - 1))
		return CO_RC(INVALID_PARAMETER);

	if (min_va < CO_ARCH_KERNEL_MAP)
		return CO_RC(INVALID_PARAMETER);

	if (ram_bytes > CO_KLOAD_MAX_RAM_BYTES) {
		co_debug_error("kload: %lld MB exceeds the %lld MB pseudo-RAM limit",
			       ram_bytes >> 20,
			       (unsigned long long)CO_KLOAD_MAX_RAM_MB);
		return CO_RC(INVALID_PARAMETER);
	}

	/*
	 * __pa(kernel_address) is now a pseudo address with phys_base zero.  The
	 * image may span any number of host allocations because p2m resolves each
	 * 4 KB page independently.
	 */
	image_hi = (max_va - CO_ARCH_KERNEL_MAP + CO_ARCH_PAGE_SIZE - 1)
		   & CO_ARCH_PAGE_MASK;
	kload_phys_base = 0;
	kload_table_start = image_hi;

	/* One PT page maps 2 MB.  Add 2 MB for the other levels and mappings. */
	sizing_bytes = ram_bytes > image_hi ? ram_bytes : image_hi;
	table_bytes = (2ULL << 20) + ((sizing_bytes + 511) >> 9);
	table_bytes = (table_bytes + CO_ARCH_PMD_SIZE - 1)
		      & ~(CO_ARCH_PMD_SIZE - 1);
	if (table_bytes < CO_KLOAD_MIN_TABLE_BYTES)
		table_bytes = CO_KLOAD_MIN_TABLE_BYTES;
	kload_table_end = kload_table_start + table_bytes;
	kload_table_top = kload_table_end;

	capacity_end = ram_bytes + table_bytes;
	if (capacity_end < kload_table_end)
		capacity_end = kload_table_end;
	capacity_end = (capacity_end + CO_ARCH_PAGE_SIZE - 1) & CO_ARCH_PAGE_MASK;

	/*
	 * The arena base, pinned NOW and never derived from kload_backed_pages:
	 * RAM backs lazily, so backed_pages grows for the machine's whole
	 * life. The loader read bounds at boot and the daemon read them
	 * seconds later; they got different answers, the guest's drm_mm and
	 * the daemon's KWINDOW_AT disagreed by the difference, and the first
	 * real MAP_BLOB landed inside RAM's address space and was refused.
	 * The base is the RAM CEILING -- address space RAM may someday back --
	 * not the RAM currently backed.
	 */
	kload_window_base_pages =
		(unsigned long)(capacity_end >> CO_ARCH_PAGE_SHIFT);

	capacity_end += CO_KLOAD_WINDOW_ARENA_BYTES;

	rc = kload_translation_alloc(
		(unsigned long)(capacity_end >> CO_ARCH_PAGE_SHIFT));
	if (!CO_OK(rc))
		return rc;

	if (!kload_grow_to(kload_table_end)) {
		co_debug_error("kload: nonpaged pool could not back the kernel image"
			       " and reserved tables page by page");
		kload_release_pages(manager);
		return CO_RC(OUT_OF_MEMORY);
	}

	co_debug("kload: pseudo image through 0x%llx, %lld MB reserved for tables;"
		 " %d cached pool block(s)",
		 image_hi, table_bytes >> 20, kload_block_count);

	/* page tables out of the block too, or Linux cannot walk them */
	co_arch_guest_space_set_frame_source(co_kload_table_frame, co_kload_frame_va);

	rc = co_arch_guest_space_create(manager, &kload_space);
	if (!CO_OK(rc)) {
		co_arch_guest_space_set_frame_source(NULL, NULL);
		kload_release_pages(manager);
		return rc;
	}

	rc = kload_map_translation_pages(manager);
	if (!CO_OK(rc)) {
		co_arch_guest_space_t* space = kload_space;

		kload_space = NULL;
		co_arch_guest_space_destroy(manager, space);
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
	co_pfn_t ppfn, mfn;
	int level = -1;
	co_rc_t rc;

	if (CO_OK(co_arch_guest_lookup(manager, kload_space, page_va, &pa, &level)) && pa) {
		*pfn_out = (co_pfn_t)(pa >> CO_ARCH_PAGE_SHIFT);
		return CO_RC(OK);
	}

	/*
	 * The pseudo frame is implied by the link address.  Its machine frame is
	 * whatever cached-pool page p2m recorded for that pseudo PFN.
	 */
	if (page_va < CO_ARCH_KERNEL_MAP)
		return CO_RC(INVALID_PARAMETER);

	pa = kload_kva_pa(page_va);
	if (kload_block_count == 0 || pa + CO_ARCH_PAGE_SIZE > kload_table_start) {
		co_debug_error("kload: 0x%llx implies pseudo frame 0x%llx, outside the"
			       " image range", page_va,
			       (unsigned long long)pa);
		return CO_RC(INVALID_PARAMETER);
	}
	ppfn = (co_pfn_t)(pa >> CO_ARCH_PAGE_SHIFT);
	if (!kload_pseudo_to_machine_pfn(ppfn, &mfn))
		return CO_RC(ERROR);

	/*
	 * Zero before mapping, not after. A section that only partly fills its
	 * last page would otherwise leave whatever the host had there visible to
	 * the guest, and .bss is nothing but that case.
	 */
	{
		void* p = co_kload_pseudo_frame_va(ppfn);

		if (p == NULL)
			return CO_RC(ERROR);
		co_memset(p, 0, CO_ARCH_PAGE_SIZE);
	}

	rc = co_arch_guest_map(manager, kload_space, page_va,
			       ((co_pa_t)mfn) << CO_ARCH_PAGE_SHIFT, _KERNPG_TABLE);
	if (!CO_OK(rc)) {
		/* the frame belongs to the block; nothing to give back */
		return rc;
	}

	kload_pages++;
	*pfn_out = mfn;

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
/*
 * Read and write guest memory through a root the caller names, for the frames
 * the load-time space cannot see.
 *
 * The pair below walk kload_space, which is the host's own load-time PML4. That
 * resolves everything the host mapped and nothing the guest mapped afterwards
 * -- and after boot handoff the guest runs on the kernel's init_top_pgt, where
 * cpu_entry_area and the whole vmalloc region (with CONFIG_VMAP_STACK, that is
 * most task stacks) live. An interrupt taken from ring 3 pushes its frame on
 * the cpu_entry_area entry stack, so reading it through the load-time root
 * returns NOT_FOUND every single time.
 *
 * The right root is the one the guest was actually running on, and the crossing
 * already captured it: linuxvm_state.cr3.
 */
static co_rc_t kload_xfer_cr3(co_manager_t* manager, unsigned long long cr3,
			      unsigned long long va, unsigned char* buf,
			      unsigned long size, bool_t write)
{
	co_pfn_t root = (co_pfn_t)((cr3 & CO_ARCH_PAGE_MASK) >> CO_ARCH_PAGE_SHIFT);

	if (!cr3)
		return CO_RC(ERROR);

	while (size) {
		unsigned long offset = (unsigned long)(va & ~CO_ARCH_PAGE_MASK);
		unsigned long part   = CO_ARCH_PAGE_SIZE - offset;
		co_pa_t pa = 0;
		int level = -1;
		unsigned char* p;

		if (part > size)
			part = size;

		if (!CO_OK(co_arch_guest_lookup_root(manager, root, va, &pa, &level))
		    || !pa)
			return CO_RC(NOT_FOUND);

		p = co_kload_frame_va((co_pfn_t)(pa >> CO_ARCH_PAGE_SHIFT));
		if (p == NULL)
			return CO_RC(ERROR);

		if (write)
			co_memcpy(p + offset, buf, part);
		else
			co_memcpy(buf, p + offset, part);

		buf  += part;
		va   += part;
		size -= part;
	}

	return CO_RC(OK);
}

co_rc_t co_kload_read_cr3(co_manager_t* manager, unsigned long long cr3,
			  unsigned long long va, unsigned char* buf,
			  unsigned long size)
{
	return kload_xfer_cr3(manager, cr3, va, buf, size, PFALSE);
}

co_rc_t co_kload_write_cr3(co_manager_t* manager, unsigned long long cr3,
			   unsigned long long va, const unsigned char* buf,
			   unsigned long size)
{
	return kload_xfer_cr3(manager, cr3, va, (unsigned char*)buf, size, PTRUE);
}

/*
 * One guest virtual address, resolved once to a host pointer.
 *
 * For callers that will read the same word again and again and cannot afford
 * a page-table walk each time -- the monitor loop's idle gate is the case this
 * exists for, where a walk or a mutex per yield would be felt.
 *
 * The pointer is only valid while the guest's address space and the blocks
 * behind it are alive, so the ONLY correct use is: resolve while the guest is
 * certainly up, and drop the pointer in the same retirement that runs before
 * co_kload_free releases anything. co_vgpu_set_address is the worked example.
 * Anything that caches this across a teardown is the 0xD5 class again.
 *
 * Single page: a caller wanting more must resolve each page, because nothing
 * guarantees two guest-virtual neighbours are host-physical neighbours.
 */
/*
 * BEACON -- grep bait for the next person, by request, after this function
 * ate its second engineer:
 *
 *   LOAD-TIME ROOT ONLY. DEAD MAP AFTER adopt_kernel_tables. NOT_FOUND
 *   QUIETLY. kernel .bss NOT RESOLVABLE HERE. guest symbol read fails
 *   silently. timer deadline bug 2026-08-11. cooperative timer refused
 *   (space.c). USE co_kload_read_cr3 WITH pp->linuxvm_state.cr3 FOR
 *   ANYTHING THE LIVE GUEST OWNS.
 *
 * This walks the address space the LOADER built. The guest replaces those
 * tables with its own mid-boot and lives there; a kernel symbol that is
 * mapped for the running guest can be absent here, and the failure is a
 * quiet NULL. Both times this was hit, the caller's code was correct, ran
 * at the right moment, and read nothing, while a probe somewhere luckier
 * reported the mechanism healthy.
 */
void* co_kload_host_ptr(co_manager_t* manager, unsigned long long va)
{
	unsigned long offset = (unsigned long)(va & ~CO_ARCH_PAGE_MASK);
	co_pa_t	      pa     = 0;
	int	      level  = -1;
	unsigned char* p;

	if (kload_space == NULL)
		return NULL;

	if (!CO_OK(co_arch_guest_lookup(manager, kload_space, va, &pa, &level)) || !pa)
		return NULL;

	p = co_kload_frame_va((co_pfn_t)(pa >> CO_ARCH_PAGE_SHIFT));
	if (p == NULL)
		return NULL;

	return p + offset;
}

/*
 * One guest virtual address to its physical address, under the teardown lock.
 *
 * For user-mode callers, which is why it locks: the walk goes through the
 * guest's page tables and those live in memory a teardown can free.
 */
co_rc_t co_kload_virt_to_phys(co_manager_t* manager, unsigned long long va,
			      co_pa_t* pa_out)
{
	co_rc_t rc = CO_RC(NOT_FOUND);
	int	level = -1;
	co_pa_t pa = 0;

	co_os_mutex_acquire(kload_lock);

	if (kload_space != NULL &&
	    CO_OK(co_arch_guest_lookup(manager, kload_space, va, &pa, &level)) && pa) {
		co_pfn_t ppfn;

		if (kload_machine_to_pseudo_pfn(
				(co_pfn_t)(pa >> CO_ARCH_PAGE_SHIFT), &ppfn)) {
			*pa_out = (((co_pa_t)ppfn) << CO_ARCH_PAGE_SHIFT)
				| (va & ~CO_ARCH_PAGE_MASK);
			rc = CO_RC(OK);
		}
	}

	co_os_mutex_release(kload_lock);
	return rc;
}

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
 * co_kload_read for a caller outside the driver, holding the teardown lock.
 *
 * co_kload_read itself takes nothing. That is correct for its in-driver callers
 * -- the monitor loop owns the guest for the duration, and taking a mutex per
 * page there would be a real regression -- but it is not correct for the KREAD
 * ioctl, which any process can issue at any moment, including while another
 * closes its handle and tears the address space down underneath it. The tables
 * being walked are inside the blocks kload_release_pages hands back, so a walk
 * that outlives the free reads pool that has been reissued: bugcheck 0xD5 under
 * Driver Verifier's special pool, and silent corruption without it. It has been
 * safe until now only because the single caller was sequenced before its own
 * teardown, which is a property of one program rather than of this interface.
 *
 * Holding kload_lock across the whole walk is what makes it safe, because
 * co_kload_free takes the same lock and does its retiring and freeing inside
 * it. The cost is that a reader delays a teardown, so the transfer is bounded:
 * CO_KREAD_MAX_BYTES caps how long any one call can hold the lock, and a
 * caller wanting more issues more calls, each of which is a fresh chance for
 * the teardown to win.
 */
co_rc_t co_kload_read_locked(co_manager_t* manager, unsigned long long va,
			     unsigned char* buf, unsigned long size)
{
	co_rc_t rc;

	if (size > CO_KREAD_MAX_BYTES)
		return CO_RC(INVALID_PARAMETER);

	co_os_mutex_acquire(kload_lock);

	/*
	 * Re-checked under the lock, not before it. Checking outside would be
	 * the same bug in a politer form: the space can be destroyed between
	 * the test and the acquire.
	 */
	if (kload_space == NULL) {
		co_os_mutex_release(kload_lock);
		return CO_RC(ERROR);
	}

	rc = co_kload_read(manager, va, buf, size);

	co_os_mutex_release(kload_lock);
	return rc;
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
 * One 32-bit word, in a single access.
 *
 * co_kload_read and co_kload_write end in memcpy, and memcpy is free to move
 * four bytes as four byte moves. For an image being loaded that is irrelevant.
 * For a ring counter it is not: the guest advances tx_head from its own
 * processor while the host reads it, so a byte-wise read can return a value
 * that never existed -- high half new, low half old. The daemon then computes
 * head - tail, gets something larger than the ring, and correctly concludes
 * the ring is corrupt, which it is not. The same tearing on the write side
 * hands the guest a tx_tail that never existed, and its fullness arithmetic
 * then permits a write that really does overwrite unread frames.
 *
 * Both windows are open only while a counter is changing, so they scale with
 * traffic: invisible on an idle guest, and reliably fatal partway through a
 * package install or a bulk copy.
 *
 * The ABI already assigns each of these words a single writer. What it needs
 * from this side is that the access is indivisible, which on x86 an aligned
 * 32-bit load or store is. Alignment is required rather than assumed -- an
 * unaligned access is only atomic by accident of not crossing a cache line.
 */
co_rc_t co_kload_read_u32(co_manager_t* manager, unsigned long long va,
			  unsigned int* out)
{
	volatile unsigned int* p;

	if (va & 3)
		return CO_RC(INVALID_PARAMETER);

	p = (volatile unsigned int*)co_kload_host_ptr(manager, va);
	if (p == NULL)
		return CO_RC(NOT_FOUND);

	*out = *p;
	return CO_RC(OK);
}

co_rc_t co_kload_write_u32(co_manager_t* manager, unsigned long long va,
			   unsigned int value)
{
	volatile unsigned int* p;

	if (va & 3)
		return CO_RC(INVALID_PARAMETER);

	p = (volatile unsigned int*)co_kload_host_ptr(manager, va);
	if (p == NULL)
		return CO_RC(NOT_FOUND);

	*p = value;
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
	unsigned long long desired_end;
	unsigned long long text_phys, end_phys;
	unsigned long long have;
	unsigned long ppfn;

	/* A failed or in-progress rebuild must never be offered to KMAP. */
	co_os_mutex_acquire(kload_lock);
	kload_ram_ready = PFALSE;
	co_os_mutex_release(kload_lock);

	if (kload_space == NULL || kload_block_count == 0)
		return CO_RC(ERROR);

	if (text_va < CO_ARCH_KERNEL_MAP || end_va <= text_va)
		return CO_RC(INVALID_PARAMETER);

	text_phys = kload_kva_pa(text_va) & CO_ARCH_PAGE_MASK;
	end_phys  = (kload_kva_pa(end_va) + CO_ARCH_PAGE_SIZE - 1) & CO_ARCH_PAGE_MASK;

	if (end_phys > kload_table_start) {
		co_debug_error("kload: image 0x%llx..0x%llx crosses the reserved tables",
			       text_phys, end_phys);
		return CO_RC(INVALID_PARAMETER);
	}

	kload_ram_bytes = ram_bytes;
	kload_text_phys = text_phys;
	kload_end_phys  = end_phys;

	/* The reserved table hole does not count against the requested usable RAM. */
	desired_end = kload_table_end;
	if (ram_bytes > kload_table_start)
		desired_end += ram_bytes - kload_table_start;
	if ((desired_end >> CO_ARCH_PAGE_SHIFT) > kload_p2m_capacity) {
		co_debug_error("kload: KRAM target exceeds the p2m capacity fixed at begin");
		return CO_RC(INVALID_PARAMETER);
	}

	if (!kload_grow_to(desired_end))
		co_debug("kload: cached nonpaged pool exhausted after %lld MB usable",
			 ((((unsigned long long)kload_backed_pages) << CO_ARCH_PAGE_SHIFT)
			  - (kload_table_end - kload_table_start)) >> 20);

	have = (((unsigned long long)kload_backed_pages) << CO_ARCH_PAGE_SHIFT)
	     - (kload_table_end - kload_table_start);
	co_debug("kload: %d cached blocks, %lld MB usable against a %lld MB target",
		 kload_block_count, have >> 20, ram_bytes >> 20);

	/*
	 * Prebuild the whole direct map at 4 KB granularity.  A large hardware
	 * leaf cannot represent scattered machine frames; the guest patch also
	 * disables PSE/GB pages so init_mem_mapping() builds the identical kind
	 * of entries when it takes ownership.
	 */
	kload_ram_pages = 0;
	for (ppfn = 0; ppfn < kload_backed_pages; ppfn++) {
		co_pfn_t mfn = kload_p2m[ppfn];
		co_pa_t pseudo = ((co_pa_t)ppfn) << CO_ARCH_PAGE_SHIFT;
		co_rc_t rc = co_arch_guest_map(manager, kload_space,
			CO_ARCH_DIRECT_MAP + pseudo,
			((co_pa_t)mfn) << CO_ARCH_PAGE_SHIFT,
			_KERNPG_TABLE | _PAGE_GLOBAL | CO_ARCH_PAGE_NX);

		if (!CO_OK(rc))
			return rc;
		kload_ram_pages++;
	}

	co_debug("kload: %lld MB of pseudo RAM, %ld independently backed pages",
		 have >> 20, kload_ram_pages);

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
	co_os_mutex_acquire(kload_lock);
	kload_ram_ready = PTRUE;
	co_os_mutex_release(kload_lock);

	return CO_RC(OK);
}

/* Convert the linked pseudo addresses in one static table to machine frames. */
static co_rc_t kload_machineize_table(unsigned long long table_va,
				      int entries, const char* what)
{
	unsigned long long* t;
	co_pa_t pseudo;
	int i, fixed = 0;

	pseudo = kload_kva_pa(table_va);
	t = (unsigned long long*)co_kload_pseudo_frame_va(
		(co_pfn_t)(pseudo >> CO_ARCH_PAGE_SHIFT));
	if (t == NULL) {
		co_debug_error("kload: %s is outside pseudo RAM", what);
		return CO_RC(ERROR);
	}

	for (i = 0; i < entries; i++) {
		co_pa_t target, machine;
		unsigned long long flags;

		if (!(t[i] & _PAGE_PRESENT))
			continue;
		if (t[i] & _PAGE_PSE) {
			co_debug_error("kload: %s still contains a large hardware leaf", what);
			return CO_RC(ERROR);
		}

		target = (co_pa_t)(t[i] & CO_KLOAD_PFN_MASK);
		flags = t[i] & ~CO_KLOAD_PFN_MASK;
		machine = kload_pseudo_to_machine_pa(target);
		if (machine == 0 && target != 0)
			return CO_RC(ERROR);
		t[i] = machine | flags;
		fixed++;
	}

	co_debug("kload: %s, %d pseudo entries translated to machine frames",
		 what, fixed);
	return CO_RC(OK);
}

co_rc_t co_kload_adopt_kernel_tables(co_manager_t* manager,
				     const unsigned long long* table_va,
				     int table_count,
				     unsigned long long* cr3_out)
{
	unsigned long long* kernel_pml4;
	unsigned long long* ours;
	unsigned long long* host_pud;
	unsigned long long* kernel_pud;
	co_pfn_t kernel_pud_mfn;
	co_pa_t pml4_pseudo, pml4_machine, fixmap_machine;
	int i, grafted = 0;

	if (kload_space == NULL || kload_block_count == 0 || table_count < 5)
		return CO_RC(ERROR);

	/*
	 * The linked level2_kernel_pgt is a row of 2 MB leaves.  Scattered host
	 * frames cannot be represented by those leaves, so the host-built 4 KB
	 * kernel-image subtree remains authoritative.  The static fixmap subtree
	 * is still needed by early_ioremap; translate its parent entries and hang
	 * it off a private copy of the host-built PUD's final slot.
	 */
	if (!CO_OK(kload_machineize_table(table_va[3], 512,
					   "level2_fixmap_pgt")))
		return CO_RC(ERROR);

	pml4_pseudo = kload_kva_pa(table_va[0]);
	kernel_pml4 = (unsigned long long*)co_kload_pseudo_frame_va(
		(co_pfn_t)(pml4_pseudo >> CO_ARCH_PAGE_SHIFT));
	if (kernel_pml4 == NULL)
		return CO_RC(ERROR);

	ours = (unsigned long long*)co_kload_frame_va(
			(co_pfn_t)(co_arch_guest_space_root(kload_space) >> CO_ARCH_PAGE_SHIFT));
	if (ours == NULL)
		return CO_RC(ERROR);

	if (!(ours[CO_ARCH_PGD_INDEX(CO_ARCH_KERNEL_MAP)] & _PAGE_PRESENT))
		return CO_RC(ERROR);
	host_pud = (unsigned long long*)co_kload_frame_va((co_pfn_t)
		((ours[CO_ARCH_PGD_INDEX(CO_ARCH_KERNEL_MAP)]
		  & CO_KLOAD_PFN_MASK) >> CO_ARCH_PAGE_SHIFT));
	if (host_pud == NULL)
		return CO_RC(ERROR);

	/*
	 * Give the live kernel root its own PUD page.
	 *
	 * Pointing init_top_pgt at host_pud and then attaching the static
	 * fixmap there would also attach that external subtree to kload_space.
	 * Its destructor quite correctly walks every table reachable from its
	 * root; it would then walk image-owned fixmap pages that were never
	 * counted as its tables. Nothing is individually freed in this mode,
	 * but the ownership mismatch is still wrong and masks real teardown
	 * errors. The clone keeps all host-built mappings while leaving the
	 * loader's tree self-contained.
	 */
	if (!CO_OK(co_kload_table_frame(manager, &kernel_pud_mfn)))
		return CO_RC(OUT_OF_MEMORY);
	kernel_pud = (unsigned long long*)co_kload_frame_va(kernel_pud_mfn);
	if (kernel_pud == NULL)
		return CO_RC(ERROR);
	co_memcpy(kernel_pud, host_pud, CO_ARCH_PAGE_SIZE);

	fixmap_machine = kload_pseudo_to_machine_pa(kload_kva_pa(table_va[3]));
	if (fixmap_machine == 0)
		return CO_RC(ERROR);
	kernel_pud[511] = fixmap_machine | _KERNPG_TABLE;

	/*
	 * init_top_pgt is the live swapper_pg_dir.  Copy every host-built kernel
	 * entry into it, including the 4 KB image, direct map, p2m/m2p metadata,
	 * boot stack and passage page.  New process PGDs clone this kernel half.
	 */
	for (i = 0; i < 512; i++) {
		if (!(ours[i] & _PAGE_PRESENT))
			continue;

		kernel_pml4[i] = ours[i];
		grafted++;
	}
	kernel_pml4[CO_ARCH_PGD_INDEX(CO_ARCH_KERNEL_MAP)] =
		(((unsigned long long)kernel_pud_mfn) << CO_ARCH_PAGE_SHIFT)
		| (ours[CO_ARCH_PGD_INDEX(CO_ARCH_KERNEL_MAP)]
		   & ~CO_KLOAD_PFN_MASK);

	pml4_machine = kload_pseudo_to_machine_pa(pml4_pseudo);
	if (pml4_machine == 0)
		return CO_RC(ERROR);
	co_debug("kload: adopted the kernel's tables, cr3 0x%llx, %d host entries grafted",
		 (unsigned long long)pml4_machine, grafted);

	*cr3_out = pml4_machine;
	return CO_RC(OK);
}

unsigned long co_kload_ram_pages(void)
{
	return kload_ram_pages;
}

/*
 * The window arena, stated by its owner. The guest transport advertises this
 * range as the virtio-gpu host-visible shm region and the daemon computes
 * MAP_BLOB offsets against its base; both read it from here so there is no
 * second party deriving the same numbers from adjacent facts (the e820 top
 * happens to equal the arena base today, and relying on that would be an
 * implicit contract).
 */
void co_kload_window_bounds(unsigned long long* base, unsigned long long* top)
{
	*base = ((unsigned long long)kload_window_base_pages) << CO_ARCH_PAGE_SHIFT;
	*top  = ((unsigned long long)kload_p2m_capacity) << CO_ARCH_PAGE_SHIFT;
}

unsigned long co_kload_p2m_pages(void)
{
	/*
	 * Capacity, not backed RAM: this value becomes the guest's
	 * co_colinux_p2m_pages, below which pseudo_to_machine consults the
	 * p2m and above which it falls back to identity. Host memory windows
	 * live between backed RAM and capacity -- report only backed pages
	 * and a guest PTE naming a window identity-maps to a machine address
	 * that does not exist, which reads as all-ones and swallows writes.
	 */
	return kload_p2m_capacity;
}

unsigned long long co_kload_m2p_mask(void)
{
	return kload_m2p_mask_value;
}
