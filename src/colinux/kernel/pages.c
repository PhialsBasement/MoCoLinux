/*
 * This source code is a part of coLinux source package.
 *
 * Dan Aloni <da-x@colinux.org>, 2004 (c)
 *
 * The code is licensed under the GPL. See the COPYING file at
 * the root directory.
 */

#include <colinux/arch/mmu.h>
#include <colinux/os/kernel/alloc.h>
#include <colinux/common/libc.h>

#include "monitor.h"
#include "pages.h"
#include "reversedpfns.h"

/*
 * co_manager_get_page - allocate a page from the host,
 * return as a PFN (page frame number).
 */

co_rc_t co_manager_get_page(struct co_manager *manager, co_pfn_t *pfn)
{
	co_rc_t rc = CO_RC_OK;

	rc = co_os_get_page(manager, pfn);
	if (!CO_OK(rc))
		return rc;

	if (*pfn >= manager->hostmem_pages) {
		/* Surprise! We have a bug! */

		co_debug_error("PFN too high! %ld >= %ld",
			 *pfn, manager->hostmem_pages);

		return CO_RC(ERROR);
	}

	return rc;
}

/*
 * co_monitor_get_pfn - Return the PFN of the physical
 * page mapped at the given guest virtual address.
 */

co_rc_t co_monitor_get_pfn(co_monitor_t *cmon, vm_ptr_t address, co_pfn_t *pfn)
{
	unsigned long current_pfn, pfn_group, pfn_index;

	current_pfn = (address >> CO_ARCH_PAGE_SHIFT);
	pfn_group = current_pfn / PTRS_PER_PTE;
	pfn_index = current_pfn % PTRS_PER_PTE;

	if (cmon->pp_pfns[pfn_group] == NULL)
		return CO_RC(ERROR);

	*pfn = cmon->pp_pfns[pfn_group][pfn_index];
	return CO_RC(OK);
}

typedef co_rc_t (*co_split_by_pages_callback_t)(
	unsigned long offset,
	unsigned long size,
	void **data
	);

co_rc_t co_split_by_pages_and_callback(
	unsigned long offset,
	unsigned long size,
	void **data,
	co_split_by_pages_callback_t func
	)
{
	unsigned long part_size;
	co_rc_t rc;

	/* Split page one by one (including partials) */
	while (size > 0) {
		part_size = ((offset + CO_ARCH_PAGE_SIZE) & CO_ARCH_PAGE_MASK) - offset;
		if (part_size > size)
			part_size = size;

		rc = func(offset, part_size, data);
		if (!CO_OK(rc))
			return rc;

		size -= part_size;
		offset += part_size;
	}

	return CO_RC(OK);
}

typedef co_rc_t (*co_manager_scan_pfns_callback_t)(
	void *mapped_ptr,
	void **data,
	unsigned long size
	);

typedef struct {
	co_manager_scan_pfns_callback_t func;
	void **data;
	co_monitor_t *monitor;
	bool_t alloc_pages;
} co_manager_scan_pfns_callback_data_t;

static co_rc_t
scan_pfns_split_callback(
	unsigned long offset,
	unsigned long size,
	void **data
	)
{
	co_manager_scan_pfns_callback_data_t *cbdata;
	unsigned long current_pfn, pfn_group;
	void *mapped_page;
	co_pfn_t real_pfn, **pp_pfns;
	co_rc_t rc;

	cbdata = (typeof(cbdata))(data);
	pp_pfns = cbdata->monitor->pp_pfns;

	current_pfn = (offset >> CO_ARCH_PAGE_SHIFT);
	pfn_group = current_pfn / PTRS_PER_PTE;
	current_pfn = current_pfn % PTRS_PER_PTE;

	if (pp_pfns[pfn_group] == NULL) {
		rc = co_monitor_malloc(cbdata->monitor, sizeof(co_pfn_t)*PTRS_PER_PTE, (void **)&pp_pfns[pfn_group]);
		if (!pp_pfns[pfn_group])
			return CO_RC(ERROR);

		co_memset(pp_pfns[pfn_group], 0, sizeof(co_pfn_t)*PTRS_PER_PTE);
	}

	if (!cbdata->alloc_pages)
		return CO_RC(OK);

	real_pfn = pp_pfns[pfn_group][current_pfn];
	if (real_pfn == 0) {
		rc = co_manager_get_page(cbdata->monitor->manager, &real_pfn);
		if (!CO_OK(rc))
			return rc;

		pp_pfns[pfn_group][current_pfn] = real_pfn;
	}

	if (cbdata->func) {
		mapped_page = co_os_map(cbdata->monitor->manager, real_pfn);
		rc = cbdata->func(mapped_page + (offset & (~CO_ARCH_PAGE_MASK)), cbdata->data, size);
		co_os_unmap(cbdata->monitor->manager, mapped_page, real_pfn);
	}

	return CO_RC(OK);
}

co_rc_t co_manager_scan_pfns_and_callback(
	co_monitor_t *monitor,
	vm_ptr_t address,
	unsigned long size,
	co_manager_scan_pfns_callback_t func,
	void **data,
	bool_t alloc_pages
	)
{
	co_manager_scan_pfns_callback_data_t cbdata;

	cbdata.func = func;
	cbdata.data = data;
	cbdata.monitor = monitor;
	cbdata.alloc_pages = alloc_pages;

	return co_split_by_pages_and_callback(address, size, (void **)&cbdata,
					      scan_pfns_split_callback);
}

static co_rc_t
copy_callback(
	void *mapped_ptr,
	void **data,
	unsigned long size
	)
{
	co_memcpy(mapped_ptr, *data, size);
	*data = (void *)(((char *)(*data)) + size);
	return CO_RC(OK);
}


/*
 * co_monitor_copy_and_create_pfns - allocate pages for
 * the given range in the address space of the guest if
 * needed, and copy the @size'ed buffer at @source to
 * the guest's memory at @address.
 */

co_rc_t
co_monitor_copy_and_create_pfns(
	co_monitor_t *monitor,
	vm_ptr_t address,
	unsigned long size,
	char *source
	)
{
	return co_manager_scan_pfns_and_callback(
		monitor, address, size,
		copy_callback,
		(void **)&source, PTRUE);
}

/*
 * co_monitor_scan_and_create_pfns - allocate pages for
 * the given range in the address space of the guest if
 * needed.
 */
co_rc_t
co_monitor_scan_and_create_pfns(
	co_monitor_t *monitor,
	vm_ptr_t address,
	unsigned long size
	)
{
	return co_manager_scan_pfns_and_callback(
		monitor, address, size,
		NULL, NULL, PTRUE);
}

static co_rc_t
create_ptes_callback(
	void *mapped_ptr,
	void **data,
	unsigned long size
	)
{
	co_pfn_t *pfns = (co_pfn_t *)(*data);
	linux_pte_t *ptes = (linux_pte_t *)mapped_ptr;
	int i;

	for (i=0; i < size/sizeof(linux_pte_t); i++) {
		if (*pfns != 0)
			*ptes = (*pfns << CO_ARCH_PAGE_SHIFT) |
			    _PAGE_PRESENT | _PAGE_RW | _PAGE_DIRTY | _PAGE_ACCESSED;
		else
			*ptes = 0;
		pfns++;
		ptes++;
	}

	*data = (void *)pfns;

	return CO_RC(OK);
}

/**
 * co_monitor_create_ptes - create a page table from a list of
 * page frame numbers.
 *
 * @monitor: monitor instnace.
 * @address: a virtual address in the guest.
 * @size: the size of the page table at @address in bytes.
 * @source: a list of PFNs.
 */

co_rc_t
co_monitor_create_ptes(
	co_monitor_t *monitor,
	vm_ptr_t address,
	unsigned long size,
	co_pfn_t *source
	)
{
	return co_manager_scan_pfns_and_callback(
		monitor, address, size,
		create_ptes_callback,
		(void **)&source, PTRUE);
}


typedef struct {
	void *data;
	co_monitor_t *monitor;
} co_monitor_copy_region_callback_data_t;

static co_rc_t
copy_region_split_callback(
	unsigned long offset,
	unsigned long size,
	void **data)
{
	co_monitor_copy_region_callback_data_t *cbdata;
	unsigned char *mapped_page;
	unsigned char *in_page;
	co_pfn_t real_pfn;
	co_rc_t rc;

	cbdata = (typeof(cbdata))(data);

	rc = co_monitor_alloc_and_map_page(cbdata->monitor, offset & CO_ARCH_PAGE_MASK);
	if (!CO_OK(rc))
		return rc;

	rc = co_monitor_get_pfn(cbdata->monitor, offset & CO_ARCH_PAGE_MASK, &real_pfn);
	if (!CO_OK(rc))
		return rc;

	mapped_page = co_os_map(cbdata->monitor->manager, real_pfn);
	in_page = mapped_page + (offset & (~CO_ARCH_PAGE_MASK));

	if (cbdata->data) {
		co_memcpy(in_page, cbdata->data, size);
		cbdata->data += size;
	} else
		co_memset(in_page, 0, size);

	co_os_unmap(cbdata->monitor->manager, mapped_page, real_pfn);

	return CO_RC(OK);
}

co_rc_t co_monitor_copy_region(
	struct co_monitor *monitor,
	vm_ptr_t address,
	unsigned long size,
	void *data_to_copy
	)
{
	co_monitor_copy_region_callback_data_t cbdata;

	cbdata.data = data_to_copy;
	cbdata.monitor = monitor;

	return co_split_by_pages_and_callback(address, size, (void **)&cbdata,
					      copy_region_split_callback);
}

/**
 * co_monitor_alloc_and_map_page - allocate a page and
 * map in the guest's virtual address space.
 */
co_rc_t co_monitor_alloc_and_map_page(
	struct co_monitor *monitor,
	vm_ptr_t address
	)
{
	co_rc_t rc;
	vm_ptr_t pte_address;
	long virtual_pfn;
	co_pfn_t physical_pfn;
	unsigned long current_pfn, pfn_group, pfn_index;

	/* first, allocate the page if needed. */

	current_pfn = (address >> CO_ARCH_PAGE_SHIFT);
	pfn_group = current_pfn / PTRS_PER_PTE;
	pfn_index = current_pfn % PTRS_PER_PTE;

	if (monitor->pp_pfns[pfn_group] == NULL) {
		rc = co_monitor_malloc(monitor, sizeof(co_pfn_t)*PTRS_PER_PTE,
				       (void **)&monitor->pp_pfns[pfn_group]);
		if (!monitor->pp_pfns[pfn_group])
			return CO_RC(OUT_OF_MEMORY);

		co_memset(monitor->pp_pfns[pfn_group], 0,
			  sizeof(co_pfn_t)*PTRS_PER_PTE);
	}

	physical_pfn = monitor->pp_pfns[pfn_group][pfn_index];
	if (physical_pfn)
		return CO_RC(OK);

	rc = co_manager_get_page(monitor->manager, &physical_pfn);
	if (!CO_OK(rc))
		return rc;

	monitor->pp_pfns[pfn_group][pfn_index] = physical_pfn;

	/* next, map the page */
	virtual_pfn = ((address - CO_ARCH_KERNEL_OFFSET) >> CO_ARCH_PAGE_SHIFT);
	pte_address = CO_VPTR_PSEUDO_RAM_PAGE_TABLES;
	pte_address += sizeof(linux_pte_t)*virtual_pfn;

	rc = co_manager_set_reversed_pfn(monitor->manager,
					 physical_pfn, virtual_pfn);
	if (!CO_OK(rc))
		return rc;

	/*
	 * pte_address is the virtual address where the PTE
	 * for this page is located. create the PTE:
	 */
	rc = co_monitor_create_ptes(monitor, pte_address,
				    sizeof(linux_pte_t), &physical_pfn);
	return rc;
}

/**
 * co_monitor_free_and_unmap - free a page and unmap
 * it from the guest's virtual address space.
 */

co_rc_t co_monitor_free_and_unmap_page(
	struct co_monitor *monitor,
	vm_ptr_t address
	)
{
	co_pfn_t physical_pfn = 0;
	vm_ptr_t pte_address;
	long virtual_pfn;
	unsigned long current_pfn, pfn_group, pfn_index;

	current_pfn = (address >> CO_ARCH_PAGE_SHIFT);
	pfn_group = current_pfn / PTRS_PER_PTE;
	pfn_index = current_pfn % PTRS_PER_PTE;

	if (monitor->pp_pfns[pfn_group] == NULL)
		return CO_RC(ERROR);

	virtual_pfn = ((address - CO_ARCH_KERNEL_OFFSET) >> CO_ARCH_PAGE_SHIFT);
	pte_address = CO_VPTR_PSEUDO_RAM_PAGE_TABLES;
	pte_address += sizeof(linux_pte_t) * virtual_pfn;

	/* erase the mapping */
	co_monitor_create_ptes(monitor, pte_address, sizeof(linux_pte_t), &physical_pfn);

	physical_pfn = monitor->pp_pfns[pfn_group][pfn_index];
	if (physical_pfn != 0) {
		co_manager_set_reversed_pfn(monitor->manager, physical_pfn, 0);
		co_os_put_page(monitor->manager, physical_pfn);
		monitor->pp_pfns[pfn_group][pfn_index] = 0;
	}

	return CO_RC(OK);
}
