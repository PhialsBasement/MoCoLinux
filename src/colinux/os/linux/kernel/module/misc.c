/*
 * This source code is a part of coLinux source package.
 *
 * Dan Aloni <da-x@colinux.org>, 2004 (c)
 *
 * The code is licensed under the GPL. See the COPYING file at
 * the root directory.
 *
 */

#include "linux_inc.h"
#include <asm/io.h>
#include <colinux/os/kernel/misc.h>

co_pa_t co_os_virt_to_phys(void *addr)
{
	return virt_to_phys(addr);
}

co_rc_t co_os_physical_memory_pages(unsigned long long *pages)
{
	*pages = num_physpages;

	/*
	 * Round to 16 MB boundars, since Linux doesn't return the
	 * exact amount but a bit lower.
	 */
	*pages = ~0xfff & ((*pages) + 0xfff);

	return CO_RC(OK);
}
