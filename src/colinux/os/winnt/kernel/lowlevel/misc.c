/*
 * This source code is a part of coLinux source package.
 *
 * Dan Aloni <da-x@colinux.org>, 2003 (c)
 *
 * The code is licensed under the GPL. See the COPYING file at
 * the root directory.
 *
 */

#include "../ddk.h"

#include <colinux/os/kernel/misc.h>

co_pa_t co_os_virt_to_phys(void *addr)
{
	PHYSICAL_ADDRESS pa;

	pa = MmGetPhysicalAddress((PVOID)addr);

	return pa.QuadPart;
}

co_rc_t co_os_physical_memory_pages(unsigned long long *pages)
{
	SYSTEM_BASIC_INFORMATION sbi;
	NTSTATUS status;
	co_rc_t rc = CO_RC(OK);

	status = NtQuerySystemInformation(SystemBasicInformation, &sbi, sizeof(sbi), NULL);
	if (status != STATUS_SUCCESS)
		return CO_RC(ERROR);

	*pages = sbi.NumberOfPhysicalPages;

	/*
	 * Round to 16 MB boundars, since Windows doesn't return the
	 * exact amount but a bit lower.
	 */
	*pages = ~0xfff & ((*pages) + 0xfff);

	return rc;
}
