
/* Linux host: allocate memory in linux kernel space */

#include "linux_inc.h"

#include <colinux/user/debug.h>
#include <colinux/os/alloc.h>
#include <colinux/os/kernel/misc.h>
#include <asm/mman.h>

#ifdef DEBUG_CO_KMALLOC
static int blocks = 0;
#endif

void *co_os_malloc(unsigned long bytes)
{
	void *ret;

	ret = kmalloc(bytes, GFP_KERNEL);

#ifdef DEBUG_CO_KMALLOC
	co_debug_lvl(allocations, 11, "BLOCK ALLOC %d: %x %d", blocks++, ret, bytes);
#endif

	return ret;
}

void co_os_free(void *ptr)
{
	kfree(ptr);

#ifdef DEBUG_CO_KMALLOC
	co_debug_lvl(allocations, 11, "BLOCK FREE %d: %x", --blocks, ptr);
#endif
}

co_rc_t co_os_userspace_map(void *address, unsigned int pages, void **user_address_out, void **handle_out)
{
	struct file *filp;
	unsigned long pa;
	void *result;

	filp = filp_open("/dev/kmem", O_RDWR | O_LARGEFILE, 0);
	if (!filp) {
		co_debug("error: co_os_userspace_map: open /dev/kmem failed");
		return CO_RC(ERROR);
	}

	pa = co_os_virt_to_phys(address);
	if (!pa) {
		co_debug("error: co_os_userspace_map: co_os_virt_to_phys failed");
		filp_close(filp, NULL);
		return CO_RC(ERROR);
	}

	result = (void *)do_mmap_pgoff(filp, 0, ((unsigned long)pages) << PAGE_SHIFT,
					     PROT_EXEC | PROT_READ | PROT_WRITE,
#if LINUX_VERSION_CODE == KERNEL_VERSION(2,6,12)
					     MAP_SHARED,
#else
					     MAP_PRIVATE,
#endif
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,13)
					     ((unsigned)__va(pa)) >> PAGE_SHIFT
#else
					     pa >> PAGE_SHIFT
#endif
	);
	if (IS_ERR(result)) {
		co_debug("error: co_os_userspace_map: do_mmap_pgoff failed (errno %ld)", PTR_ERR(result));
		filp_close(filp, NULL);
		return CO_RC(ERROR);
	}

	*user_address_out = result;
	*handle_out = filp;

	return CO_RC(OK);
}

void co_os_userspace_unmap(void *user_address, void *handle, unsigned int pages)
{
	struct file *filp = (struct file *)handle;

	if (user_address)
		do_munmap(current->mm, (unsigned long)user_address, ((unsigned long)pages) << PAGE_SHIFT);

	filp_close(filp, NULL);
}

/*
 * The multi-allocator probe is a Windows-host porting aid: it exists to find an
 * allocator whose pages are executable, because the default Windows one is not.
 * Linux as host has no such problem, so only the default method is offered.
 */
bool_t co_os_alloc_method(int index, const char** name)
{
	if (index != 0)
		return PFALSE;
	if (name)
		*name = "__get_free_pages";
	return PTRUE;
}

void* co_os_alloc_pages_by(int index, unsigned int pages)
{
	if (index != 0)
		return NULL;
	return co_os_alloc_pages(pages);
}

void co_os_free_pages_by(int index, void* ptr, unsigned int pages)
{
	if (index == 0)
		co_os_free_pages(ptr, pages);
}

int co_os_exec_alloc_index(void)
{
	return 0;
}

void* co_os_alloc_exec_pages(unsigned int pages)
{
	return co_os_alloc_pages(pages);
}

void co_os_free_exec_pages(void* ptr, unsigned int pages)
{
	co_os_free_pages(ptr, pages);
}

/*
 * The modern pseudo-physical loader is a Windows-host path today.  Keep the
 * semantic allocator available to the Linux host build; its existing page
 * allocator remains the implementation until that host grows a vmap-backed
 * KMAP path.
 */
void* co_os_alloc_cached_pages(unsigned int pages)
{
	return co_os_alloc_pages(pages);
}

void co_os_free_cached_pages(void* ptr, unsigned int pages)
{
	co_os_free_pages(ptr, pages);
}
