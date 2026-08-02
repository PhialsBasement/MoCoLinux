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

/*
 * One descriptor of the guest's scatter-gather list: a physical run and its
 * length. Must match struct cobd_sg in the guest's drivers/block/cobd.c.
 */
typedef struct {
	unsigned long long pa;
	unsigned int	   len;
	unsigned int	   pad;
} co_cobd_sg_t;

/*
 * A whole request in one crossing: the descriptors say where the scattered
 * pages are, and the file offset advances across them.
 *
 * The guest used to cross once per page, because page-cache pages are not
 * physically contiguous and the block layer therefore hands over one segment
 * per page. The crossing is what costs, so a 128 KB readahead cost thirty-two
 * of them with a synchronous host read inside each.
 *
 * Every descriptor is read out of guest memory through the frame lookup, and
 * every transfer it describes goes through the same per-page resolution as
 * before -- so an address the guest invents still cannot reach host memory.
 */
co_rc_t co_cobd_request_sg(co_manager_t* manager, int unit,
			   unsigned long long offset,
			   unsigned long long sg_pa, unsigned int count,
			   bool_t write)
{
	unsigned int i;

	if (unit < 0 || unit >= CO_COBD_MAX_UNITS || !cobd_unit[unit].dev)
		return CO_RC(INVALID_PARAMETER);

	if (count == 0 || count > CO_COBD_MAX_SG)
		return CO_RC(INVALID_PARAMETER);

	for (i = 0; i < count; i++) {
		unsigned long long dpa = sg_pa + (unsigned long long)i
					       * sizeof(co_cobd_sg_t);
		co_cobd_sg_t	   d;
		unsigned long	   got = 0;
		co_rc_t		   rc;

		/*
		 * Copied a page at a time, because a descriptor CAN straddle
		 * one -- and the comment that used to sit here saying it could
		 * not was the single most expensive line in this file.
		 *
		 * The array is sixteen-byte objects but it is only eight-byte
		 * aligned: blk_mq_rq_to_pdu puts it at rq + sizeof(struct
		 * request), and that is 248 bytes in this build. So roughly one
		 * descriptor in every 256 lands at page offset 4088, with its
		 * pa in this page and its len in the next one.
		 *
		 * Reading it as one struct through a single frame lookup is
		 * right for as long as those two pages are adjacent in the
		 * host's address space, which they are within one allocation
		 * block and are not across a boundary between two -- guest RAM
		 * is a set of separate 32 MB host allocations. At a boundary
		 * the second half of the descriptor is read out of whatever the
		 * host put next to that block.
		 *
		 * What that produces is the worst possible combination: a
		 * correct destination address with a garbage length. It is not
		 * rejected -- the address is real guest memory -- so
		 * co_cobd_request below walks that length writing file contents
		 * across the guest until it runs off the end of its RAM. Under
		 * a heavy install that means files that come back as binary
		 * noise (pacman reporting "unknown key" for entries in its own
		 * database, hooks with unreadable option lines) and, when what
		 * it lands on is kernel text or a page table, a triple fault
		 * with no bugcheck and nothing to read.
		 */
		while (got < sizeof(d)) {
			unsigned long long at  = dpa + got;
			unsigned long	   off = (unsigned long)(at & (CO_ARCH_PAGE_SIZE - 1));
			unsigned long	   part = CO_ARCH_PAGE_SIZE - off;
			unsigned char*	   dva;

			if (part > sizeof(d) - got)
				part = sizeof(d) - got;

			dva = (unsigned char*)co_kload_frame_va(
				(co_pfn_t)(at >> CO_ARCH_PAGE_SHIFT));
			if (!dva) {
				co_debug_error("cobd%d: sg list pa 0x%llx is not"
					       " guest memory", unit, at);
				return CO_RC(ERROR);
			}

			co_memcpy((unsigned char*)&d + got, dva + off, part);
			got += part;
		}

		/*
		 * And believe nothing about the length. Even read correctly it
		 * comes from guest memory, and the loop it feeds writes for as
		 * long as it says. One segment cannot exceed what the guest's
		 * own queue limit allows.
		 */
		if (d.len == 0 || d.len > CO_COBD_MAX_SEGMENT) {
			co_debug_error("cobd%d: sg descriptor %u has length %u,"
				       " refusing the request", unit, i, d.len);
			return CO_RC(INVALID_PARAMETER);
		}

		rc = co_cobd_request(manager, unit, offset, d.pa, d.len, write);
		if (!CO_OK(rc))
			return rc;

		offset += d.len;
	}

	return CO_RC(OK);
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
