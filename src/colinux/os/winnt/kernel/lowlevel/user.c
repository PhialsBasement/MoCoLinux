/*
 * This source code is a part of coLinux source package.
 *
 * Dan Aloni <da-x@colinux.org>, 2004 (c)
 *
 * The code is licensed under the GPL. See the COPYING file at
 * the root directory.
 *
 */

#include "../ddk.h"

#include <excpt.h>

#include <colinux/common/libc.h>
#include <colinux/os/alloc.h>
#include <colinux/os/kernel/user.h>

co_rc_t co_copy_to_user(char *user_address, char *kernel_address, unsigned long size)
{
	PMDL user_mdl;

	if (size == 0)
		return CO_RC(OK);

	user_mdl = IoAllocateMdl(user_address, size, FALSE, FALSE, NULL);
	if (user_mdl) {
		void *vptr;
		MmProbeAndLockPages(user_mdl, KernelMode, IoWriteAccess);
		vptr = MmMapLockedPagesSpecifyCache(user_mdl, KernelMode, MmCached, NULL, FALSE, LowPagePriority);
		if (vptr != NULL) {
			co_memcpy(vptr, kernel_address, size);
			MmUnmapLockedPages(vptr, user_mdl);
		}
		MmUnlockPages(user_mdl);
		IoFreeMdl(user_mdl);
		return CO_RC(OK);
	}

	return CO_RC(ERROR);
}


/*
 * Lock a caller's pages down and report the machine frames behind them.
 *
 * Unlike the copy helpers above, this one keeps the lock: the frames are about
 * to be published in the guest's p2m, so the guest will hold page-table
 * entries naming them. A page that Windows moved or reused underneath that
 * would not fault -- it would silently hand the guest, and the GPU, somebody
 * else's memory. The MDL is returned as an opaque handle and the pages stay
 * locked until it is released.
 *
 * The frames are copied rather than aliased: PFN_NUMBER is a Windows type and
 * co_pfn_t is ours, and they are only incidentally the same width.
 */
co_rc_t co_os_user_lock_pages(void *user_address, unsigned long size,
			      void **handle_out, co_pfn_t **pfns_out,
			      unsigned long *count_out)
{
	PMDL user_mdl;
	PPFN_NUMBER mdl_pfns;
	co_pfn_t *pfns;
	unsigned long pages, i;

	if (handle_out == NULL || pfns_out == NULL || count_out == NULL)
		return CO_RC(INVALID_PARAMETER);

	*handle_out = NULL;
	*pfns_out   = NULL;
	*count_out  = 0;

	if (size == 0 || (size & (CO_ARCH_PAGE_SIZE - 1)) != 0)
		return CO_RC(INVALID_PARAMETER);

	/*
	 * Cheap sanity before the lock. MmProbeAndLockPages raises on a bad
	 * range, and this toolchain has no SEH to catch it with -- the copy
	 * helpers above have always called it bare for the same reason. The
	 * caller is the GPU daemon rather than arbitrary user code, so this
	 * checks for the mistakes a caller can plausibly make rather than
	 * defending against a hostile one.
	 */
	if (user_address == NULL ||
	    ((uintptr_t)user_address & (CO_ARCH_PAGE_SIZE - 1)) != 0)
		return CO_RC(INVALID_PARAMETER);

	user_mdl = IoAllocateMdl(user_address, size, FALSE, FALSE, NULL);
	if (user_mdl == NULL)
		return CO_RC(OUT_OF_MEMORY);

	MmProbeAndLockPages(user_mdl, KernelMode, IoModifyAccess);

	pages = size >> CO_ARCH_PAGE_SHIFT;
	pfns  = (co_pfn_t *)co_os_malloc(pages * sizeof(co_pfn_t));
	if (pfns == NULL) {
		MmUnlockPages(user_mdl);
		IoFreeMdl(user_mdl);
		return CO_RC(OUT_OF_MEMORY);
	}

	mdl_pfns = MmGetMdlPfnArray(user_mdl);
	for (i = 0; i < pages; i++)
		pfns[i] = (co_pfn_t)mdl_pfns[i];

	*handle_out = user_mdl;
	*pfns_out   = pfns;
	*count_out  = pages;
	return CO_RC(OK);
}

void co_os_user_unlock_pages(void *handle)
{
	PMDL user_mdl = (PMDL)handle;

	if (user_mdl == NULL)
		return;

	MmUnlockPages(user_mdl);
	IoFreeMdl(user_mdl);
}

co_rc_t co_copy_from_user(char *user_address, char *kernel_address, unsigned long size)
{
	PMDL user_mdl;

	if (size == 0)
		return CO_RC(OK);

	user_mdl = IoAllocateMdl(user_address, size, FALSE, FALSE, NULL);
	if (user_mdl) {
		void *vptr;
		MmProbeAndLockPages(user_mdl, KernelMode, IoWriteAccess);
		vptr = MmMapLockedPagesSpecifyCache(user_mdl, KernelMode, MmCached, NULL, FALSE, LowPagePriority);
		if (vptr != NULL) {
			co_memcpy(kernel_address, vptr, size);
			MmUnmapLockedPages(vptr, user_mdl);
		}
		MmUnlockPages(user_mdl);
		IoFreeMdl(user_mdl);
		return CO_RC(OK);
	}

	return CO_RC(ERROR);
}
