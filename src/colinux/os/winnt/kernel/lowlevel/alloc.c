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

/*
 * Measured on XP x64: NonPagedPool comes back NX-clear and cached, where
 * MmAllocateNonCachedMemory comes back NX-set and uncached. Physical contiguity
 * is not required -- the passage code takes virt_to_phys of each sub-structure
 * separately -- so there is no reason to spend contiguous memory on it.
 * Allocations of PAGE_SIZE or more are page-aligned, which the page-table
 * sub-structures need.
 */
int co_os_exec_alloc_index(void)
{
	return CO_ALLOC_NONPAGED_POOL;
}

void* co_os_alloc_exec_pages(unsigned int pages)
{
	if (pages == 0)
		return NULL;

	return co_os_alloc_pages_by(CO_ALLOC_NONPAGED_POOL, pages);
}

void co_os_free_exec_pages(void* ptr, unsigned int pages)
{
	if (ptr == NULL)
		return;

	co_os_free_pages_by(CO_ALLOC_NONPAGED_POOL, ptr, pages);
}

void* co_os_alloc_cached_pages(unsigned int pages)
{
	if (pages == 0)
		return NULL;

	return co_os_alloc_pages_by(CO_ALLOC_NONPAGED_POOL, pages);
}

void co_os_free_cached_pages(void* ptr, unsigned int pages)
{
	if (ptr == NULL)
		return;

	co_os_free_pages_by(CO_ALLOC_NONPAGED_POOL, ptr, pages);
}

void* co_os_alloc_contiguous_pages(unsigned int pages)
{
	if (pages == 0)
		return NULL;

	return co_os_alloc_pages_by(CO_ALLOC_CONTIGUOUS, pages);
}

void co_os_free_contiguous_pages(void* ptr, unsigned int pages)
{
	if (ptr == NULL)
		return;

	co_os_free_pages_by(CO_ALLOC_CONTIGUOUS, ptr, pages);
}

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

/*
 * What the last user mapping looked like to the driver, one instruction after
 * making it. Read out through the kmap ioctl; see the note at the query below.
 */
unsigned long co_last_map_state;
unsigned long co_last_map_protect;

/*
 * Defined in build/safe-map.c and linked directly into linux.sys.
 *
 * It cannot live in this compilation unit: this tree combines driver objects
 * with two `ld -r` passes, and GNU ld corrupts x64 .pdata addends while doing
 * that. Ordinary functions survive because they rarely raise; an SEH wrapper
 * without its runtime-function entry is exactly the unhandled exception it is
 * meant to prevent. Keeping the wrapper out of both aggregate objects lets the
 * final PE link consume its .pdata/.xdata once, without an intermediate link.
 */
extern PVOID co_os_map_locked_pages_user_safe(PMDL mdl);

co_rc_t co_os_userspace_map(void *address, unsigned int pages, void **user_address_out, void **handle_out)
{
	void *user_address = NULL;
	/*
	 * SIZE_T, not unsigned long. Windows is LLP64, so `unsigned long` is 32
	 * bits here and this shift overflows silently at 4 GB of pages -- the
	 * same truncation class that has bitten vm_ptr_t, the host ISR address
	 * and CO_ARCH_PAGE_MASK in this tree already. Nothing asks for that much
	 * today; the type is correct so that nothing has to notice when it does.
	 */
	SIZE_T memory_size = ((SIZE_T)pages) << CO_ARCH_PAGE_SHIFT;
	PMDL mdl;

	mdl = IoAllocateMdl(address, (ULONG)memory_size, FALSE, FALSE, NULL);
	if (!mdl)
		return CO_RC(ERROR);

	/*
	 * MmBuildMdlForNonPagedPool, which is what this is for.
	 *
	 * A hand-rolled fill lived here briefly -- MmGetPhysicalAddress per page
	 * plus MDL_PAGES_LOCKED set by hand -- on the theory that this routine is
	 * only valid for pool memory and guest RAM comes from
	 * MmAllocateContiguousMemory. That theory was wrong twice over.
	 *
	 * Microsoft states it plainly: "For mappings to user space, MDLs that are
	 * built by the MmBuildMdlForNonPagedPool routine can be used." It was
	 * never the reason 8.1 failed -- that was a 32-bit truncation of the
	 * returned address in co_manager_kmap, and this call was innocent
	 * throughout. Replacing it fixed nothing and was never tested on the
	 * hosts that already worked.
	 *
	 * It also bugchecked XP: opening one application was enough. An MDL whose
	 * flags claim locked pages the memory manager did not lock is a lie it
	 * acts on, and setting MDL_PAGES_LOCKED by hand skips the bookkeeping the
	 * real routine does. Reverted, and left with this note so the idea is not
	 * had again.
	 */
	MmBuildMdlForNonPagedPool(mdl);

	user_address = co_os_map_locked_pages_user_safe(mdl);

	if (!user_address) {
		co_debug("userspace_map: Windows refused %lu KB; mapping not installed",
			 (unsigned long)(memory_size >> 10));
		IoFreeMdl(mdl);
		return CO_RC(ERROR);
	}

	*handle_out = (void *)mdl;

	/*
	 * The address Windows gave, not one recomputed from it.
	 *
	 * This used to return PAGE_ALIGN(user_address) + MmGetMdlByteOffset(mdl),
	 * which is only the same value when the MDL's byte offset agrees with the
	 * low bits of the mapping -- and it does not have to.
	 * MmMapLockedPagesSpecifyCache already returns a pointer to the start of
	 * the described data, so rounding it down to a page and adding the offset
	 * back is at best a no-op and at worst moves the pointer off the mapping
	 * entirely. Windows 8.1 is where that showed: the daemon was handed 129
	 * addresses and VirtualQuery found free address space at every one of
	 * them, then took an access violation on the first read.
	 *
	 * There is nothing to reconstruct. The mapping starts where the function
	 * says it starts.
	 */
	*user_address_out = user_address;

	/*
	 * Check the mapping is there, from inside the driver, at the moment it
	 * is made.
	 *
	 * The daemon receives every address the driver reports and VirtualQuery
	 * finds free address space at all of them -- first range, last range,
	 * alike. Two possibilities remain and they need different fixes: the map
	 * never happened in this process, or it happened and was undone before
	 * the ioctl returned. Asking one instruction after the call separates
	 * them, and that is the whole point of this block.
	 *
	 * MmIsAddressValid, not ZwQueryVirtualMemory: the latter is not in the
	 * set MmGetSystemRoutineAddress will resolve for a driver, so the first
	 * attempt at this silently reported state 0 and answered nothing.
	 * MmIsAddressValid is exported on every NT and needs no prototype games.
	 * It reports only whether a read would fault right now, which is exactly
	 * the question, and it is safe at PASSIVE_LEVEL on a user address in the
	 * current process.
	 *
	 * co_debug goes to the colinux debug channel, which needs a reader
	 * nobody is running, so the answer also rides back through the ioctl in
	 * co_last_map_* where the daemon can print it.
	 */
	{
		BOOLEAN here = MmIsAddressValid(user_address);

		co_debug("kmap slice: user %p valid=%d (%lu pages)",
			 user_address, (int)here, (unsigned long)pages);

		co_last_map_state   = here ? 1 : 0;
		co_last_map_protect = (unsigned long)pages;
	}

	return CO_RC(OK);
}

void co_os_userspace_unmap(void *user_address, void *handle, unsigned int pages)
{
	PMDL mdl = (PMDL)handle;

	if (user_address)
		MmUnmapLockedPages(user_address, mdl);

	IoFreeMdl(mdl);
}
