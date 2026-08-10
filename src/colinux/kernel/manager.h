/*
 * This source code is a part of coLinux source package.
 *
 * Dan Aloni <da-x@colinux.org>, 2003 (c)
 *
 * The code is licensed under the GPL. See the COPYING file at
 * the root directory.
 */

#ifndef __COLINUX_KERNEL_MANAGER_H__
#define __COLINUX_KERNEL_MANAGER_H__

#include <colinux/arch/manager.h>
#include <colinux/arch/mmu.h>
#include <colinux/common/queue.h>

#include "debug.h"

typedef struct co_osdep_manager *co_osdep_manager_t;

typedef enum {
	CO_MANAGER_STATE_NOT_INITIALIZED,
	CO_MANAGER_STATE_INITIALIZED_DEBUG,
	CO_MANAGER_STATE_INITIALIZED_ARCH,
	CO_MANAGER_STATE_INITIALIZED_OSDEP,
	CO_MANAGER_STATE_INITIALIZED,
} co_manager_state_t;

struct co_monitor;
struct co_manager_open_desc_os;

typedef struct co_manager_open_desc_os *co_manager_open_desc_os_t;

typedef struct co_manager_kmap_slice {
	struct co_manager_kmap_slice *next;
	void		 *handle;	/* the MDL */
	void		 *user_va;
	unsigned long	  pages;
	unsigned long long pa;		/* guest pseudo-physical range */
	unsigned long long bytes;
} co_manager_kmap_slice_t;

/* One host-memory window published into the guest; see KWINDOW. */
typedef struct co_manager_window {
	struct co_manager_window *next;
	void		 *handle;	/* the MDL holding the pages down */
	unsigned long	  pages;
	unsigned long long pseudo_pa;	/* where the guest sees them */
} co_manager_window_t;

/*
 * Stamped at open, cleared immediately before the descriptor is freed.
 *
 * A descriptor that reaches a close path without this is one that has already
 * been released -- and the evidence that this happens is a bugcheck, not a
 * theory: 0x50 four times in one day, every one of them ExFreePool faulting
 * on the pointer at offset 0x48 of a descriptor whose memory had already been
 * recycled, reached from co_os_manager_userspace_close. Handing a recycled
 * field to the pool allocator destroys the evidence along with the machine;
 * refusing and saying so leaves both intact.
 */
#define CO_MANAGER_OPEN_MAGIC 0x4f50454e444f4353ULL	/* "OPENDOCS" */

typedef struct co_manager_open_desc {
	co_list_t node;

	unsigned long long magic;

	bool_t active;
	int ref_count;
	co_os_mutex_t lock;

	bool_t monitor_owner;
	struct co_monitor *monitor;

	co_queue_t out_queue;

	co_manager_open_desc_os_t os;
	co_debug_section_t *debug_section;

	/*
	 * This handle's user-mode windows onto guest RAM, if any.
	 *
	 * Recorded here rather than in the mapper because the mapping belongs
	 * to the handle, not to the request: a process that maps and then dies
	 * without unmapping has to be cleaned up by whoever closes its handle,
	 * and that is IRP_MJ_CLEANUP -- which is the one close path that runs
	 * in the dying process's own context, the context MmUnmapLockedPages
	 * requires for a UserMode mapping. IRP_MJ_CLOSE can run in an arbitrary
	 * process and must not be the one to do it.
	 *
	 * A linked list, not a fixed array: KMAP_RANGE adds working-set slices on
	 * demand and a 128 GB guest must not allocate a response-sized block in
	 * nonpaged pool just because one texture was touched. Nodes are immutable
	 * after publication and are all retired by KUNMAP/cleanup.
	 */
	co_manager_kmap_slice_t *kmap_slice;
	unsigned long		  kmap_slices;
	bool_t			  kmap_reserved;

	/*
	 * Host memory windows published into the guest (KWINDOW), tracked for
	 * the same reason the kmap slices above are: the guest holds page-table
	 * entries naming these machine frames, so a process that dies without
	 * releasing them would leave the guest -- and the GPU -- pointed at
	 * memory Windows has re-issued to something else. Cleanup on handle
	 * close clears the p2m entries first, then unlocks the pages.
	 */
	struct co_manager_window *window;
	unsigned long		  windows;
} *co_manager_open_desc_t;

/*
 * The manager module manages the running coLinux systems.
 */
typedef struct co_manager {
	co_manager_state_t state;

	/*
	 * 64-bit because "unsigned long" is 32 bits on Windows x64, and these
	 * are byte counts and page counts for the whole machine. The old
	 * "more than 4GB is not supported" check made that safe by refusing to
	 * load at all; with the check gone, hostmem_usage_limit <<= 20 on an
	 * 8 GB box would overflow and silently come back as about 3.9 GB.
	 */
	unsigned long long hostmem_amount;
	unsigned long long hostmem_used;
	unsigned long long hostmem_usage_limit;
	unsigned long long hostmem_pages;

	co_pfn_t *reversed_map_pfns;
	unsigned long reversed_page_count;
	unsigned long *reversed_map_pgds;
	unsigned long reversed_map_pgds_count;

	co_osdep_manager_t osdep;
	co_archdep_manager_t archdep;

	co_manager_debug_t debug;

	co_list_t monitors;
	unsigned long monitors_count;

	co_list_t opens;
	unsigned long num_opens;
	co_os_mutex_t lock;
} co_manager_t;

extern co_manager_t *co_global_manager;

extern co_rc_t co_manager_load(co_manager_t *manager);

extern co_rc_t co_manager_ioctl(co_manager_t *manager, unsigned long ioctl,
				void *io_buffer, unsigned long in_size,
				unsigned long out_size, unsigned long *return_size,
				co_manager_open_desc_t opened);

extern co_rc_t co_manager_send_eof(co_manager_t *manager, co_manager_open_desc_t opened);
extern co_rc_t co_manager_send(co_manager_t *manager, co_manager_open_desc_t opened, co_message_t *message);
extern co_rc_t co_manager_open(co_manager_t *manager, co_manager_open_desc_t *opened_out);
extern co_rc_t co_manager_open_ref(co_manager_open_desc_t opened);
extern co_rc_t co_manager_open_desc_deactive_and_close(co_manager_t *manager, co_manager_open_desc_t opened);

/*
 * Drop every window this handle holds. Runs at IRP_MJ_CLEANUP, and is a no-op
 * for the overwhelming majority of handles, which never map anything.
 */
extern void co_manager_kmap_release(co_manager_t *manager,
				    co_manager_open_desc_t opened);

/*
 * Host-memory windows published into the guest (KWINDOW). Release clears the
 * p2m entries before unlocking the pages, so the guest can never hold a
 * page-table entry naming a frame Windows has taken back.
 */
extern void co_manager_window_track(co_manager_open_desc_t opened,
				    void *handle,
				    unsigned long long pseudo_pa,
				    unsigned long pages);
extern co_rc_t co_manager_window_release(co_manager_open_desc_t opened,
					 unsigned long long pseudo_pa,
					 unsigned long pages);
extern void co_manager_window_release_all(co_manager_open_desc_t opened);
extern co_rc_t co_manager_close(co_manager_t *manager, co_manager_open_desc_t opened);

extern void co_manager_unload(co_manager_t *manager);

#endif
