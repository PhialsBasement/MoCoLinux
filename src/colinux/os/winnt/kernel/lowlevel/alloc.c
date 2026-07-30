/*
 * This source code is a part of coLinux source package.
 *
 * Dan Aloni <da-x@colinux.org>, 2003 (c)
 *
 * The code is licensed under the GPL. See the COPYING file at
 * the root directory.
 *
 */

/* WinNT host: allocate pages in WinNT kernel space for
 * CPL0 driver 'linux.sys'.
 */

#include "../ddk.h"

#include <colinux/os/alloc.h>
#include <colinux/os/kernel/alloc.h>

#ifdef DEBUG_CO_OS_ALLOC
static int allocs;
static int alloc_reenter;

#define co_debug_allocations(fmt, ...) do { \
        if (!alloc_reenter) { \
		alloc_reenter++; \
		co_debug_lvl(allocations, 11, fmt, ## __VA_ARGS__); \
		alloc_reenter--; \
	} \
} while (0)
#endif

void *co_os_alloc_pages(unsigned int pages)
{
	void *ret;

	if (pages == 0)
		KeBugCheck(0x11117777);

	ret = MmAllocateNonCachedMemory(pages * CO_ARCH_PAGE_SIZE);

#ifdef DEBUG_CO_OS_ALLOC
	if (ret) {
		allocs++;
		co_debug_allocations("PAGE ALLOC %d(%u) - %p", allocs, pages, ret);
	}
#endif

	return ret;
}

void co_os_free_pages(void *ptr, unsigned int pages)
{
	if (ptr == 0)
		KeBugCheck(0x11117777 + 1);

	if (pages == 0)
		KeBugCheck(0x11117777 + 2);

#ifdef DEBUG_CO_OS_ALLOC
	co_debug_allocations("PAGE FREE %d(%u) - %p", allocs, pages, ptr);
	allocs--;
#endif
	MmFreeNonCachedMemory(ptr, pages * CO_ARCH_PAGE_SIZE);
}

#define CO_OS_PROBE_TAG (('c' << 0) | ('o' << 8) | ('p' << 16) | ('b' << 24))

/*
 * Candidate page allocators.
 *
 * MmAllocateNonCachedMemory is what coLinux has always used, and on XP x64 it
 * returns pages with NX set and PWT|PCD set -- neither executable nor cached,
 * measured on hardware. The passage code has to execute from such a page, which
 * is why arch/i386/antinx.c exists: it clears NX by editing the host's live page
 * tables. That is a four-level walk on x86-64 and exactly the tampering
 * PatchGuard watches for, so the intent here is to find an allocator that hands
 * back executable memory in the first place.
 *
 * NonPagedPool is the interesting candidate: NonPagedPoolNx did not exist until
 * Windows 8, so on XP x64 the non-paged pool should be executable and cached.
 * Allocations of PAGE_SIZE or more are page-aligned, which is what is needed --
 * physical contiguity is not, since the passage code takes virt_to_phys of each
 * sub-structure separately.
 */
enum {
	CO_ALLOC_NONCACHED = 0,		/* MmAllocateNonCachedMemory -- the default */
	CO_ALLOC_NONPAGED_POOL,
	CO_ALLOC_CONTIGUOUS,
	CO_ALLOC_METHODS
};

static const char* co_alloc_method_names[CO_ALLOC_METHODS] = {
	"MmAllocateNonCachedMemory",
	"ExAllocatePool(NonPagedPool)",
	"MmAllocateContiguousMemory",
};

bool_t co_os_alloc_method(int index, const char** name)
{
	if (index < 0 || index >= CO_ALLOC_METHODS)
		return PFALSE;
	if (name)
		*name = co_alloc_method_names[index];
	return PTRUE;
}

void* co_os_alloc_pages_by(int index, unsigned int pages)
{
	unsigned long bytes = pages * CO_ARCH_PAGE_SIZE;

	switch (index) {
	case CO_ALLOC_NONCACHED:
		return MmAllocateNonCachedMemory(bytes);

	case CO_ALLOC_NONPAGED_POOL:
		return ExAllocatePoolWithTag(NonPagedPool, bytes, CO_OS_PROBE_TAG);

	case CO_ALLOC_CONTIGUOUS: {
		PHYSICAL_ADDRESS highest;

		highest.QuadPart = ~0ULL;
		return MmAllocateContiguousMemory(bytes, highest);
	}
	}

	return NULL;
}

void co_os_free_pages_by(int index, void* ptr, unsigned int pages)
{
	if (ptr == NULL)
		return;

	switch (index) {
	case CO_ALLOC_NONCACHED:
		MmFreeNonCachedMemory(ptr, pages * CO_ARCH_PAGE_SIZE);
		break;
	case CO_ALLOC_NONPAGED_POOL:
		ExFreePool(ptr);
		break;
	case CO_ALLOC_CONTIGUOUS:
		MmFreeContiguousMemory(ptr);
		break;
	}
}

#define CO_OS_POOL_TAG (('c' << 0) | ('o' << 8) |  ('l' << 16) | ('x' << 24))

void *co_os_malloc(unsigned long bytes)
{
	void *ret;

	if (bytes == 0)
		KeBugCheck(0x11117777 + 3);

	ret = ExAllocatePoolWithTag(NonPagedPool, bytes, CO_OS_POOL_TAG);


#ifdef DEBUG_CO_OS_ALLOC
	if (ret) {
		allocs++;
		co_debug_allocations("MEM ALLOC %d(%lu) - %p", allocs, bytes, ret);
	}
#endif

	return ret;
}

void co_os_free(void *ptr)
{
#ifdef DEBUG_CO_OS_ALLOC
	co_debug_allocations("MEM FREE %d - %p", allocs, ptr);
	allocs--;
#endif
	if (ptr == 0)
		KeBugCheck(0x11117777 + 4);

	ExFreePoolWithTag(ptr, CO_OS_POOL_TAG);
}

co_rc_t co_os_userspace_map(void *address, unsigned int pages, void **user_address_out, void **handle_out)
{
	void *user_address;
	unsigned long memory_size = ((unsigned long)pages) << CO_ARCH_PAGE_SHIFT;
	PMDL mdl;

	mdl = IoAllocateMdl(address, memory_size, FALSE, FALSE, NULL);
	if (!mdl)
		return CO_RC(ERROR);

	MmBuildMdlForNonPagedPool(mdl);
	user_address = MmMapLockedPagesSpecifyCache(mdl, UserMode, MmCached, NULL, FALSE, HighPagePriority);
	if (!user_address) {
		IoFreeMdl(mdl);
		return CO_RC(ERROR);
	}

	*handle_out = (void *)mdl;
	*user_address_out = PAGE_ALIGN(user_address) + MmGetMdlByteOffset(mdl);

	return CO_RC(OK);
}

void co_os_userspace_unmap(void *user_address, void *handle, unsigned int pages)
{
	PMDL mdl = (PMDL)handle;

	if (user_address)
		MmUnmapLockedPages(user_address, mdl);

	IoFreeMdl(mdl);
}
