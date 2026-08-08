/*
 * This source code is a part of coLinux source package.
 *
 * Dan Aloni <da-x@colinux.org>, 2003 (c)
 *
 * The code is licensed under the GPL. See the COPYING file at
 * the root directory.
 *
 */

#ifndef __CO_OS_KERNEL_ALLOC_H__
#define __CO_OS_KERNEL_ALLOC_H__

#include <colinux/os/alloc.h>
#include <colinux/kernel/manager.h>
#include <colinux/arch/current/mmu.h>

/*
 * Interfaces for physical memory allocation.
 */
extern co_rc_t co_os_get_page(struct co_manager *manager, co_pfn_t *pfn);
extern void *co_os_map(struct co_manager *manager, co_pfn_t pfn);
extern void co_os_unmap(struct co_manager *manager, void *ptr, co_pfn_t pfn);
extern void co_os_put_page(struct co_manager *manager, co_pfn_t pfn);
/*
 * Page allocators, enumerated so the passage page's requirements can be measured
 * rather than assumed. The passage code executes from the page it lives in, so
 * the allocation has to be executable; the default one on Windows is not, and is
 * uncached besides. co_os_alloc_method() walks the available methods by index.
 *
 * Index 0 is always what co_os_alloc_pages() itself uses.
 */
/*
 * Pages the passage code can execute from: executable and, ideally, cached.
 * Separate from co_os_alloc_pages() on purpose -- that one also backs the
 * pseudo-physical page tables, and there is no reason to change the caching of
 * memory the CPU walks during a switch as a side effect of this.
 */
extern void* co_os_alloc_exec_pages(unsigned int pages);
extern void  co_os_free_exec_pages(void* ptr, unsigned int pages);

/*
 * Cached, non-pageable pages with one contiguous kernel virtual mapping.
 *
 * Unlike co_os_alloc_contiguous_pages(), these pages have no physical
 * contiguity requirement.  The kload pseudo-physical layer records the
 * machine frame behind each page separately; the virtual mapping is retained
 * so host I/O and KMAP can still address a block without creating cache aliases.
 */
extern void* co_os_alloc_cached_pages(unsigned int pages);
extern void  co_os_free_cached_pages(void* ptr, unsigned int pages);

/* Which co_os_alloc_method() index co_os_alloc_exec_pages() uses. */
extern int   co_os_exec_alloc_index(void);

extern bool_t co_os_alloc_method(int index, const char** name);
/* Physically contiguous pages for legacy probes and callers that truly need it. */
extern void*  co_os_alloc_contiguous_pages(unsigned int pages);
extern void   co_os_free_contiguous_pages(void* ptr, unsigned int pages);

extern void*  co_os_alloc_pages_by(int index, unsigned int pages);
extern void   co_os_free_pages_by(int index, void* ptr, unsigned int pages);

extern void *co_os_alloc_pages(unsigned int pages);
extern void co_os_free_pages(void *ptr, unsigned int pages);

/*
 * Interfaces for mapping physical memory in userspace.
 */
extern co_rc_t co_os_userspace_map(void *address, unsigned int pages, void **user_address, void **handle);
extern void co_os_userspace_unmap(void *user_address, void *handle, unsigned int pages);

#endif
