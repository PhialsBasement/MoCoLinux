/*
 * This source code is a part of coLinux source package.
 *
 * Dan Aloni <da-x@colinux.org>, 2003 (c)
 *
 * The code is licensed under the GPL. See the COPYING file at
 * the root directory.
 */

#ifndef __CO_KERNEL_MONITOR_H__
#define __CO_KERNEL_MONITOR_H__

#include <colinux/common/queue.h>
#include <colinux/common/config.h>
#include <colinux/common/import.h>
#include <colinux/common/ioctl.h>
#include <colinux/common/console.h>
#include <colinux/common/messages.h>
#include <colinux/arch/current/mmu.h>
#include <colinux/os/kernel/mutex.h>
#include <colinux/os/kernel/wait.h>
#include <colinux/os/timer.h>

struct co_monitor_device;
struct co_monitor;
struct co_manager_open_desc;

typedef co_rc_t (*co_monitor_service_func_t)(struct co_monitor *cmon,
					     struct co_monitor_device *device,
					     co_param_t *params);
typedef struct co_monitor_device {
	co_monitor_service_func_t service;
	unsigned long state;
	void *data;
	co_queue_t out_queue;
	co_queue_t in_queue;
	co_os_mutex_t mutex;
} co_monitor_device_t;

typedef enum {
	CO_MONITOR_STATE_EMPTY,
	CO_MONITOR_STATE_INITIALIZED,
	CO_MONITOR_STATE_RUNNING,
	CO_MONITOR_STATE_STARTED,
	CO_MONITOR_STATE_TERMINATED,
} co_monitor_state_t;

#define CO_MONITOR_MODULES_COUNT CO_MODULES_MAX
/*
 * We use the following struct for each coLinux system.
 */

typedef struct co_monitor {
	/*
	 * Pointer back to the manager.
	 */
	struct co_manager* manager;
	int                refcount;
	bool_t 		   listed_in_manager;
	co_list_t	   node;
	co_id_t		   id;

	/*
	 * OS-dependant data:
	 */
	struct co_monitor_osdep* osdep;

	/*
	 * State of monitor.
	 */
	co_monitor_state_t		  state;
	co_termination_reason_t		  termination_reason;
	co_monitor_linux_bug_invocation_t bug_info;

	/*
	 * Configuration data.
	 */
	co_config_t config;

	/*
	 * The passage page
	 */
	struct co_arch_passage_page* passage_page;       /* The virtual address of the
						            PP in the host */
	vm_ptr_t		     passage_page_vaddr; /* The virtual address of the
						            PP in Linux */
	struct co_archdep_monitor*   archdep;            /* Architecture dependent data */

	/*
	 * Core stuff (Linux kernel image)
	 */
	vm_ptr_t	    core_vaddr;  /* Where the core sits (the famous C0100000) */
	vm_ptr_t	    core_end;    /* Where the core ends */
	unsigned long	    core_pages;  /* number of pages our core takes */
	co_symbols_import_t import;      /* Addresse of symbols in the kernel */

	/*
	 * Pseudo physical RAM
	 */
	unsigned long memory_size;      /* The size of Linux's pseudo physical RAM */
	unsigned long physical_frames;  /* The number of pages in that RAM */
	unsigned long end_physical;     /* In what virtual address the map of the
					   pseudo physical RAM ends */
	co_pfn_t** pp_pfns;

	/*
	 * Dynamic allocations in the host
	 */
	unsigned long blocks_allocated;

	/*
	 * Page global directory
	 */
	linux_pgd_t pgd;             /* Pointer to the physical address PGD of
					Linux (to be put in CR3 eventually */
	co_pfn_t**  pgd_pfns;


	/*
	 * Devices
	 */
	co_monitor_device_t devices[CO_DEVICES_TOTAL];
	bool_t		    timer_interrupt;

        /*
         * Timer
         */
        co_os_timer_t	   timer;
	co_timestamp_t	   timestamp;
	co_timestamp_t	   timestamp_freq;
	unsigned long long timestamp_reminder;
	co_os_wait_t	   idle_wait;

	/*
	 * Video devices
	*/
	struct co_video_dev* video_devs[CO_MODULE_MAX_COVIDEO];

	/*
	 * SCSI devices
	 */
	struct co_scsi_dev* scsi_devs[CO_MODULE_MAX_COSCSI];

	/*
	 * Block devices
	 */
	struct co_block_dev* block_devs[CO_MODULE_MAX_COBD];

	/*
	 * File Systems
	 */
	struct co_filesystem* filesystems[CO_MODULE_MAX_COFS];

	/*
	 * Audio devices
	*/
	struct co_audio_dev* audio_devs[CO_MODULE_MAX_COAUDIO];

	/*
	 * Message passing stuff
	 */
	co_queue_t	linux_message_queue;
	co_os_mutex_t 	linux_message_queue_mutex;

	co_io_buffer_t* 		 io_buffer;
	co_monitor_user_kernel_shared_t* shared;
	void*			         shared_user_address;
	void*				 shared_handle;

	struct co_manager_open_desc* connected_modules[CO_MONITOR_MODULES_COUNT];
	co_os_mutex_t 		     connected_modules_write_lock;

	co_console_t* console;

        /*
	 * initrd
	 */
	unsigned long initrd_address;
	unsigned long initrd_size;

	/*
	 * Structures copied directly from the vmlinux file before it
	 * is loaded in order to get kernel-specific info (such as
	 * segment registers) and API version.
	 */
	co_info_t 	info;
	co_arch_info_t 	arch_info;
} co_monitor_t;


extern co_rc_t co_monitor_create(struct co_manager *manager, co_manager_ioctl_create_t *params, co_monitor_t **cmon_out);
extern co_rc_t co_monitor_refdown(co_monitor_t *cmon, bool_t user_context, bool_t monitor_owner);

struct co_manager_open_desc;

extern co_rc_t co_monitor_ioctl(co_monitor_t*		     cmon,
				co_manager_ioctl_monitor_t*  io_buffer,
				unsigned long		     in_size,
				unsigned long		     out_size,
				unsigned long*		     return_size,
				struct co_manager_open_desc* opened_manager);

extern co_rc_t co_monitor_alloc_pages(co_monitor_t *cmon, unsigned long pages, void **address);
extern co_rc_t co_monitor_free_pages(co_monitor_t *cmon, unsigned long pages, void *address);

extern co_rc_t co_monitor_malloc(co_monitor_t *cmon, unsigned long bytes, void **ptr);
extern co_rc_t co_monitor_free(co_monitor_t *cmon, void *ptr);

extern co_rc_t co_monitor_message_from_user(co_monitor_t *monitor, co_message_t *message);
extern co_rc_t co_monitor_message_from_user_free(co_monitor_t *monitor, co_message_t *message);

/* support kernel mode conet module */
extern co_rc_t co_conet_register_protocol(co_monitor_t *monitor);
extern co_rc_t co_conet_unregister_protocol(co_monitor_t *monitor);

extern co_rc_t co_conet_bind_adapter(co_monitor_t* monitor,
				     int	   conet_unit,
				     char*	   netcfg_id,
				     int	   promise,
				     char 	   mac[6]);

extern co_rc_t co_conet_unbind_adapter(co_monitor_t *monitor, int conet_unit);
extern co_rc_t co_conet_inject_packet_to_adapter(co_monitor_t *monitor, int conet_unit, void *packet_data, int length);

/*
 * An accessors to values of our core's kernel symbols.
 */
#define CO_MONITOR_KERNEL_SYMBOL(name)		        \
	((void *)(((char *)cmon->core_image) +	        \
		  (cmon->import.kernel_##name -	\
		   cmon->core_vaddr)))

#endif
