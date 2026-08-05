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

	/*
	 * Probe before mapping, because the map cannot be caught if it fails.
	 *
	 * MmMapLockedPagesSpecifyCache with AccessMode == UserMode RAISES an
	 * exception when it cannot map; it does not return NULL, and the
	 * BugCheckOnFailure argument is documented as ignored for UserMode. The
	 * NULL test below therefore never runs on the path that actually
	 * fails, and an unhandled exception in a driver is a bugcheck
	 * (0x1E / 0x7E), not an error return.
	 *
	 * It cannot be guarded with this toolchain: GCC's C frontend has no
	 * __try/__except -- that is MSVC syntax -- and mingw's x64 substitute
	 * (__try1/__except1 in excpt.h) is inline assembly emitting
	 * .seh_handler scope tables by hand. Getting that subtly wrong corrupts
	 * the stack on precisely the path meant to save the machine.
	 *
	 * So instead of catching the failure, make it not happen: ask the
	 * caller's address space for a reservation of exactly this size first.
	 * If there is no contiguous hole that big, the reservation fails
	 * cleanly with a status code and this returns an error, which is what
	 * the caller wanted all along. If there is, it is released again
	 * immediately and the map takes it.
	 *
	 * The window between the release and the map is real, and it is
	 * accepted knowingly: the only caller that maps at this scale is the
	 * daemon's single-threaded startup, nothing else in the process is
	 * reserving address space at that moment, and the alternative is
	 * hand-written SEH. It converts "certain bugcheck when VA runs out"
	 * into "clean refusal, with a vanishingly small chance of the old
	 * behaviour". R3 maps ~1 GB in 130 slices on every daemon start, which
	 * is what makes this path reachable at all.
	 *
	 * MEM_RESERVE only -- no pages are committed, so this costs address
	 * space for the length of the call and nothing else.
	 */
	{
		PVOID	 probe = NULL;
		SIZE_T	 probe_size = memory_size;
		NTSTATUS status;

		status = ZwAllocateVirtualMemory(ZwCurrentProcess(), &probe, 0,
						 &probe_size, MEM_RESERVE,
						 PAGE_READWRITE);
		if (!NT_SUCCESS(status)) {
			co_debug("userspace_map: no %lu KB of free user VA (status %x)",
				 (unsigned long)(memory_size >> 10),
				 (unsigned int)status);
			IoFreeMdl(mdl);
			return CO_RC(ERROR);
		}

		probe_size = 0;	/* MEM_RELEASE requires zero, with the base */
		ZwFreeVirtualMemory(ZwCurrentProcess(), &probe, &probe_size,
				    MEM_RELEASE);
	}

	/*
	 * The NULL check stays regardless: a Windows that returns NULL rather
	 * than raising must not be treated as success.
	 */
	user_address = MmMapLockedPagesSpecifyCache(mdl, UserMode, MmCached,
						    NULL, FALSE, HighPagePriority);

	if (!user_address) {
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
