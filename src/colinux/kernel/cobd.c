/*
 * This source code is a part of coLinux source package.
 *
 * The code is licensed under the GPL. See the COPYING file at
 * the root directory.
 */

#include <colinux/common/common.h>
#include <colinux/common/libc.h>
#include <colinux/common/debug.h>
#include <colinux/kernel/cobd.h>
#include <colinux/kernel/kload.h>
#include <colinux/os/kernel/blockio.h>
#include <colinux/arch/mmu.h>

typedef struct {
	co_os_bdev_t	   dev;
	unsigned long long size;
} co_cobd_unit_t;

static co_cobd_unit_t cobd_unit[CO_COBD_MAX_UNITS];

co_rc_t co_cobd_attach(int unit, const char* path, unsigned long long* size_out)
{
	co_rc_t rc;

	if (unit < 0 || unit >= CO_COBD_MAX_UNITS)
		return CO_RC(INVALID_PARAMETER);

	if (cobd_unit[unit].dev) {
		co_os_bdev_close(cobd_unit[unit].dev);
		cobd_unit[unit].dev  = NULL;
		cobd_unit[unit].size = 0;
	}

	rc = co_os_bdev_open(path, &cobd_unit[unit].dev, &cobd_unit[unit].size);
	if (!CO_OK(rc))
		return rc;

	co_debug("cobd%d: %llu bytes (%llu MB) at '%s'", unit,
		 cobd_unit[unit].size, cobd_unit[unit].size >> 20, path);

	*size_out = cobd_unit[unit].size;

	return CO_RC(OK);
}

void co_cobd_detach_all(void)
{
	int i;

	for (i = 0; i < CO_COBD_MAX_UNITS; i++) {
		if (cobd_unit[i].dev) {
			co_os_bdev_close(cobd_unit[i].dev);
			cobd_unit[i].dev  = NULL;
			cobd_unit[i].size = 0;
		}
	}
}

bool_t co_cobd_present(int unit)
{
	if (unit < 0 || unit >= CO_COBD_MAX_UNITS)
		return PFALSE;

	return cobd_unit[unit].dev != NULL;
}

unsigned long long co_cobd_size(int unit)
{
	if (unit < 0 || unit >= CO_COBD_MAX_UNITS)
		return 0;

	return cobd_unit[unit].size;
}

co_rc_t co_cobd_request(co_manager_t* manager, int unit,
			unsigned long long offset, unsigned long long guest_pa,
			unsigned long size, bool_t write)
{
	if (unit < 0 || unit >= CO_COBD_MAX_UNITS || !cobd_unit[unit].dev)
		return CO_RC(INVALID_PARAMETER);

	if (size == 0)
		return CO_RC(OK);

	/*
	 * Walked one page at a time rather than handed over whole.
	 *
	 * The guest's memory is several separate host allocations, so a buffer
	 * that is contiguous in guest physical memory is only contiguous in the
	 * host's virtual address space while it stays inside one block. Two
	 * pages either side of a block boundary have unrelated host addresses,
	 * and a single transfer across that boundary would run off the end of
	 * one allocation into whatever the host put next to it -- writing into
	 * memory belonging to Windows, from a request the guest chose the
	 * length of.
	 *
	 * Resolving each page separately also confines every transfer to
	 * memory that belongs to the guest: an address the frame lookup does
	 * not recognise is refused rather than translated to something nearby.
	 */
	while (size > 0) {
		unsigned long long page_off = guest_pa & (CO_ARCH_PAGE_SIZE - 1);
		unsigned long	   chunk    = CO_ARCH_PAGE_SIZE - (unsigned long)page_off;
		unsigned char*	   va;
		co_rc_t		   rc;

		if (chunk > size)
			chunk = size;

		va = (unsigned char*)co_kload_frame_va(
			(co_pfn_t)(guest_pa >> CO_ARCH_PAGE_SHIFT));
		if (!va) {
			co_debug_error("cobd%d: guest pa 0x%llx is not guest memory",
				       unit, guest_pa);
			return CO_RC(ERROR);
		}

		rc = co_os_bdev_rw(cobd_unit[unit].dev, offset, va + page_off,
				   chunk, write);
		if (!CO_OK(rc))
			return rc;

		guest_pa += chunk;
		offset	 += chunk;
		size	 -= chunk;
	}

	return CO_RC(OK);
}
