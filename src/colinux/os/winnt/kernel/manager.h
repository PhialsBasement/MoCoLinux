/*
 * This source code is a part of coLinux source package.
 *
 * Dan Aloni <da-x@colinux.org>, 2003 (c)
 *
 * The code is licensed under the GPL. See the COPYING file at
 * the root directory.
 *
 */

#ifndef __CO_WINNT_KERNEL_MANAGER_H__
#define __CO_WINNT_KERNEL_MANAGER_H__

#include <colinux/os/kernel/manager.h>
#include <colinux/os/kernel/alloc.h>
#include <colinux/os/kernel/mutex.h>

#include "ddk.h"

#define PFN_ALLOCATION_COUNT       (64)
#define PFN_HASH_SIZE              (0x1000)

typedef enum {
	CO_OS_PFN_PTR_TYPE_MDL,
	CO_OS_PFN_PTR_TYPE_PAGE,
} co_os_pfn_ptr_type_t;

struct co_os_mdl_ptr;

typedef struct co_os_mdl_ptr {
	PMDL mdl;
	int use_count;
	struct co_os_pfn_ptr *pfn_ptrs;
	co_list_t node;
} co_os_mdl_ptr_t;

typedef struct co_os_pfn_ptr {
	co_os_pfn_ptr_type_t type;
	co_pfn_t pfn;
	union {
		co_os_mdl_ptr_t *mdl;
		void *page;
	};
	co_list_t node;
	union {
		co_list_t unused;
		co_list_t mapped_allocated_node;
	};
} co_os_pfn_ptr_t;

#define PFN_HASH(pfn) (pfn % PFN_HASH_SIZE)

struct co_osdep_manager {
	co_os_mutex_t mutex;
	co_list_t pages_hash[PFN_HASH_SIZE]; /* array of lists of co_os_pfn_ptr_t */
	co_list_t pages_unused;
	co_list_t mdl_list;
	co_list_t mapped_allocated_list;

	unsigned long mdls_allocated;
	unsigned long pages_allocated;
	unsigned long pages_mapped;
	unsigned long long hostmem_max_physical_address;
};

#define fixme_IoSetCancelRoutine(Irp, func) \
	Irp->CancelRoutine = func;


/*
 * Stamped at open, cleared immediately before the block is freed.
 *
 * This exists because the block's address is what ExFreePool faults on: five
 * bugchecks in one evening, all 0x50, all reading pool metadata from
 * co_os_manager_userspace_close's call to co_os_free. A pointer that does not
 * carry this word is not a block this driver allocated, and handing it to the
 * pool allocator ends the machine and the investigation together.
 */
#define CO_OPEN_OS_MAGIC 0x4f50454e4f534d31ULL	/* "OPENOSM1" */

struct co_manager_open_desc_os {
	unsigned long long magic;
	PIRP irp;
};

extern void co_winnt_free_all_pages(co_osdep_manager_t osdep);

#endif
