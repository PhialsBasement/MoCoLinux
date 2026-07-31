/*
 * This source code is a part of coLinux source package.
 *
 * Dan Aloni <da-x@colinux.org>, 2003 (c)
 *
 * The code is licensed under the GPL. See the COPYING file at
 * the root directory.
 */

#ifndef __COLINUX_COMMON_IOCTL_H__
#define __COLINUX_COMMON_IOCTL_H__

#include "common.h"

#include <colinux/common/import.h>
#include <colinux/common/config.h>


typedef enum {
	CO_MANAGER_IOCTL_BASE=0x10,

	CO_MANAGER_IOCTL_CREATE,
	CO_MANAGER_IOCTL_MONITOR,
	CO_MANAGER_IOCTL_STATUS,
	CO_MANAGER_IOCTL_DEBUG,
	CO_MANAGER_IOCTL_DEBUG_READER,
	CO_MANAGER_IOCTL_DEBUG_LEVELS,
	CO_MANAGER_IOCTL_INFO,
	CO_MANAGER_IOCTL_ATTACH,
	CO_MANAGER_IOCTL_MONITOR_LIST,
	CO_MANAGER_IOCTL_PROBE_VA,
	CO_MANAGER_IOCTL_PROBE_PASSAGE,
	CO_MANAGER_IOCTL_SAVE_STATE,
	CO_MANAGER_IOCTL_TEST_SWITCH,
	CO_MANAGER_IOCTL_TEST_ROUNDTRIP,
	CO_MANAGER_IOCTL_TEST_FAULT,
	CO_MANAGER_IOCTL_TEST_RESUME,
	CO_MANAGER_IOCTL_TEST_SPACE,
	CO_MANAGER_IOCTL_TEST_PAGEFAULT,
	CO_MANAGER_IOCTL_TEST_BADSTACK,
	CO_MANAGER_IOCTL_TEST_GUEST,
	CO_MANAGER_IOCTL_TEST_GUESTFAULT,
	CO_MANAGER_IOCTL_KLOAD_BEGIN,
	CO_MANAGER_IOCTL_KLOAD_CHUNK,
	CO_MANAGER_IOCTL_KLOAD_VERIFY,
	CO_MANAGER_IOCTL_KLOAD_ENTER,
	CO_MANAGER_IOCTL_KLOAD_END,
	CO_MANAGER_IOCTL_KCALL,
	CO_MANAGER_IOCTL_KBOOT,
	CO_MANAGER_IOCTL_KRAM,
} co_manager_ioctl_t;

/*
 * This struct is mapped both in kernel space and userspace.
 */
typedef struct {
	unsigned long userspace_msgwait_count;
} co_monitor_user_kernel_shared_t;

/* interface for CO_MANAGER_IOCTL_CREATE: */
typedef struct {
	co_rc_t		    rc;
	co_symbols_import_t import;
	co_config_t	    config;
	co_info_t	    info;
	co_arch_info_t	    arch_info;
	unsigned long	    actual_memsize_used;
	void*		    shared_user_address;
	co_id_t		    id;
} co_manager_ioctl_create_t;

/*
 * ioctls()s under CO_MANAGER_IOCTL_MONITOR:
 */
typedef enum {
	CO_MONITOR_IOCTL_CLOSE,
	CO_MONITOR_IOCTL_LOAD_SECTION,
	CO_MONITOR_IOCTL_START,
	CO_MONITOR_IOCTL_RUN,
	CO_MONITOR_IOCTL_STATUS,
	CO_MONITOR_IOCTL_LOAD_INITRD,
	CO_MONITOR_IOCTL_GET_CONSOLE,  /* Get console dimentions and max buffer size */
	CO_MONITOR_IOCTL_GET_STATE,
	CO_MONITOR_IOCTL_RESET,
	CO_MONITOR_IOCTL_VIDEO_ATTACH, /* incomplete */
	CO_MONITOR_IOCTL_VIDEO_DETACH, /* incomplete */
	CO_MONITOR_IOCTL_CONET_BIND_ADAPTER,
	CO_MONITOR_IOCTL_CONET_UNBIND_ADAPTER
} co_monitor_ioctl_op_t;

/* interface for CO_MANAGER_IOCTL_MONITOR: */
typedef struct {
	co_rc_t			rc;
	co_monitor_ioctl_op_t	op;
	/*
	 * Zero-length, not a C99 flexible array: this struct is embedded as the
	 * leading member of the per-operation structs below, so a flexible array
	 * here is not at the end of the enclosing struct and GCC rejects it.
	 */
	char			extra_data[0];
} co_manager_ioctl_monitor_t;

/* interface for CO_MANAGER_IOCTL_STATUS: */
typedef struct {
	unsigned long	state; /* co_manager_state_t */
	unsigned long	reserved;
	int		monitors_count;
	int		periphery_api_version;
	int		linux_api_version;
	char		compile_time[28];
} co_manager_ioctl_status_t;

/* interface for CO_MANAGER_IOCTL_INFO: */
typedef struct {
	unsigned long hostmem_usage_limit;
	unsigned long hostmem_used;
} co_manager_ioctl_info_t;

#define CO_MANAGER_ATTACH_MAX_MODULES 0x10

/* interface for CO_MANAGER_IOCTL_ATTACH: */
typedef struct {
	co_rc_t		rc;
	co_id_t		id;
	int		num_modules;
	co_module_t	modules[CO_MANAGER_ATTACH_MAX_MODULES];
} co_manager_ioctl_attach_t;

/* interface for CO_MANAGER_IOCTL_DEBUG_READER: */
typedef struct {
	co_rc_t       rc;
	void*         user_buffer;
	unsigned long user_buffer_size;
	unsigned long filled;
} co_manager_ioctl_debug_reader_t;

#ifdef COLINUX_DEBUG
/* interface for CO_MANAGER_IOCTL_DEBUG_LEVELS: */
typedef struct {
	co_debug_levels_t levels;
	bool_t            modify;
} co_manager_ioctl_debug_levels_t;
#endif

/*
 * interface for CO_MANAGER_IOCTL_PROBE_VA
 *
 * Walks the host's own page tables for one virtual address and reports what is
 * mapped at each level. Used to answer a design question the x86-64 passage code
 * hinges on: whether the address window the guest wants is free in the host's
 * kernel address space too. If it is, the passage page can be mapped at the same
 * virtual address on both sides, which makes other_map zero and removes the need
 * for the passage code to relocate its own instruction pointer between mappings.
 *
 * Read-only. It maps each page-table page, reads one entry, and unmaps it.
 */
#define CO_PROBE_VA_LEVELS 4

typedef struct {
	co_rc_t		   rc;
	unsigned long long va;		/* in:  address to walk		*/
	unsigned long long cr3;		/* out: host CR3 at probe time	*/
	unsigned long long entry[CO_PROBE_VA_LEVELS];	/* out: PML4,PDPT,PD,PT	*/
	int		   levels_walked;	/* out: how far it got	*/
	int		   present;	/* out: PTRUE if fully mapped	*/
	int		   large_page;	/* out: stopped at a 2M/1G page	*/
	int		   supported;	/* out: PFALSE on architectures
					   without a 4-level walker	*/
} co_manager_ioctl_probe_va_t;

/*
 * interface for CO_MANAGER_IOCTL_PROBE_PASSAGE
 *
 * Allocates a passage page exactly as the monitor would, reports where the host
 * put it and what its own page tables say about it, then frees it. Two things are
 * being asked:
 *
 *  - the host virtual address, because we do not choose it. If the guest can map
 *    the page at the same address, other_map is zero and the switch never has to
 *    relocate its own instruction pointer -- and, more importantly, no host page
 *    table has to be modified to arrange it.
 *
 *  - whether the page is executable. The passage code runs from it. i386 deals
 *    with Windows marking such allocations NX by hand-clearing the bit in the
 *    host's live page tables (arch/i386/antinx.c), which is a four-level walk on
 *    x86-64 and precisely the page-table tampering to avoid. If NX comes back
 *    clear there is nothing to solve; if it is set, the allocator has to change
 *    before any switch assembly is worth writing.
 *
 * Allocates and frees. Executes nothing, writes no page tables, touches no CR3.
 */
#define CO_PROBE_ALLOC_MAX 4

typedef struct {
	char			    name[40];	/* out: allocator name		*/
	unsigned long long	    va;		/* out: host virtual address	*/
	unsigned long long	    pa;		/* out: physical address	*/
	int			    ok;		/* out: allocation succeeded	*/
	int			    used;	/* out: this is the one the passage
						   page actually allocates from	*/
	co_manager_ioctl_probe_va_t walk;	/* out: page-table walk of va	*/
} co_probe_alloc_result_t;

typedef struct {
	co_rc_t			rc;
	int			pages;	/* out: pages requested per attempt	*/
	int			count;	/* out: allocators reported		*/
	co_probe_alloc_result_t	result[CO_PROBE_ALLOC_MAX];
} co_manager_ioctl_probe_passage_t;

/*
 * interface for CO_MANAGER_IOCTL_SAVE_STATE
 *
 * Capture the host's CPU state into a co_arch_state_stack_t and hand it back.
 * Reads only. The save half of the world switch, exercised on its own so that
 * the state layout and the values the restore half must reproduce are known
 * before any of it is written.
 */
typedef struct {
	co_rc_t			rc;
	int			supported;
	int			restore;	/* in: also put the state back	*/
	co_arch_state_stack_t	state;
} co_manager_ioctl_save_state_t;

/*
 * interface for CO_MANAGER_IOCTL_TEST_SWITCH
 *
 * Change CR3 into an address space containing only the passage page, store a
 * sentinel from code executing there, and change back. Tests the one property the
 * whole passage design rests on: that execution continues across a CR3 write
 * because the code page is mapped at the same address in both address spaces.
 *
 * The full result struct lives in arch/switch.h; this carries a copy of it.
 */
typedef struct {
	co_rc_t	rc;
	int	supported;
	int	succeeded;
	unsigned long long passage_va;
	unsigned long long passage_pa;
	unsigned long long code_va;
	unsigned long long host_cr3;
	unsigned long long guest_cr3;
	unsigned long long expected;
	unsigned long long observed;
	unsigned long long guest_gdt;
	unsigned long long guest_idt;
	unsigned long long fault_handler;
	unsigned long long fault_rip;
	int		   faulted;
	unsigned long	   code_size;
	int		   iterations;
	unsigned long long counter;
	unsigned long long reg_accum;
	unsigned long long guest_stubs;
	unsigned long long guest_tss;
	unsigned long long ist_stack;
	unsigned long long guest_text;
	unsigned long long guest_stack;
	unsigned long	   tables;
	unsigned long long vector;
	unsigned long long error_code;
	unsigned long long cr2;
	int		   preflight_checked;
	int		   preflight_failed;
	int		   preflight_level;
	unsigned long long preflight_va;
} co_manager_ioctl_test_switch_t;

/* interface for the CO_MANAGER_IOCTL_KLOAD_* family */
typedef struct {
	co_rc_t		   rc;
	unsigned long long min_va;
	unsigned long long max_va;
} co_manager_ioctl_kload_begin_t;

typedef struct {
	co_rc_t		   rc;
	unsigned long long va;
	unsigned long	   size;
	int		   zero;	/* allocate the pages but write nothing */
	unsigned char	   data[0];
} co_manager_ioctl_kload_chunk_t;

typedef struct {
	co_rc_t		   rc;
	unsigned long long va;
	unsigned long	   size;
	unsigned long long checksum;
	unsigned long	   pages;
	unsigned long	   chunks;
	unsigned long	   tables;
} co_manager_ioctl_kload_verify_t;

/* interface for CO_MANAGER_IOCTL_KRAM: give the guest physical memory */
typedef struct {
	co_rc_t		   rc;
	unsigned long long ram_bytes;	/* in */
	unsigned long long text_va;	/* in */
	unsigned long long end_va;	/* in */
	unsigned long	   ram_pages;	/* out: pages allocated for RAM */
	unsigned long	   tables;	/* out */
} co_manager_ioctl_kram_t;

/* interface for CO_MANAGER_IOCTL_KBOOT */
typedef struct {
	co_rc_t		   rc;
	int		   supported;
	/* in */
	unsigned long long entry_va;
	unsigned long long initial_code_va;
	unsigned long long start_kernel_va;
	unsigned long long early_console_va;
	unsigned long long colinux_console_va;
	unsigned long long ring_symbol_va;
	unsigned long long guest_flag_va;
	int		   max_switches;
	int		   step;
	/* out */
	unsigned long long guest_cr3;
	unsigned long	   tables;
	unsigned long long console_ring_va;
	unsigned long long console_written;
	unsigned long long console_capacity;
	char		   console_text[2048];
	int		   faulted;
	int		   returned_voluntarily;
	int		   hit_limit;
	int		   unforwardable;
	int		   vmx_present;
	unsigned long	   switches;
	unsigned long	   interrupts;
	unsigned long	   steps;
	unsigned long	   trace_next;
	unsigned long long trace[16];
	unsigned long long vector;
	unsigned long long fault_rip;
	unsigned long long error_code;
	unsigned long long cr2;
	int		   preflight_checked;
	int		   preflight_failed;
	int		   preflight_level;
	unsigned long long preflight_va;
} co_manager_ioctl_kboot_t;

/* interface for CO_MANAGER_IOCTL_KCALL: mirrors co_arch_kcall_test_t */
typedef struct {
	co_rc_t		   rc;
	int		   supported;
	int		   succeeded;
	unsigned long long memset_va;		/* in, then echoed back */
	unsigned long long strlen_va;		/* in, then echoed back */
	unsigned long long scratch_va;
	unsigned long long memset_ret;
	unsigned long long strlen_ret;
	unsigned long long strlen_expected;
	unsigned long long snprintf_va;		/* in, then echoed back */
	unsigned long long snprintf_ret;
	unsigned long long snprintf_expected;
	int		   pattern_ok;
	int		   text_ok;
	char		   text[128];
	unsigned long long early_printk_va;	/* in */
	unsigned long long early_console_va;	/* in */
	unsigned long long colinux_console_va;	/* in */
	unsigned long long ring_symbol_va;	/* in */
	unsigned long long console_ring_va;
	unsigned long long console_written;
	unsigned long long console_capacity;
	int		   console_ok;
	char		   console_text[160];
	int		   first_bad;
	int		   first_bad_byte;
	int		   faulted;
	unsigned long long vector;
	unsigned long long fault_rip;
	unsigned long long cr2;
} co_manager_ioctl_kcall_t;

/* interface for CO_MANAGER_IOCTL_TEST_SPACE: mirrors co_arch_space_test_t */
typedef struct {
	co_rc_t		   rc;
	int		   supported;
	int		   succeeded;
	unsigned long long root;
	unsigned long	   mapped;
	unsigned long	   verified;
	unsigned long	   mismatched;
	unsigned long	   tables;
	unsigned long long first_bad_va;
	unsigned long long first_bad_expect;
	unsigned long long first_bad_got;
	int		   unmapped_reported;
	int		   unmapped_level;
} co_manager_ioctl_test_space_t;

/* interface for CO_MANAGER_IOCTL_MONITOR_LIST: */
typedef struct {
	co_rc_t       rc;
	unsigned long count;
	co_id_t       ids[CO_MAX_MONITORS];
} co_manager_ioctl_monitor_list_t;

/*
 * Monitor ioctl()s
 */

/* interface for CO_MONITOR_IOCTL_LOAD_SECTION: */
typedef struct {
	co_manager_ioctl_monitor_t pc;
	char*			   user_ptr;
	/* 64-bit: these are guest kernel addresses, and LLP64 `unsigned long`
	 * on the Windows host is only 4 bytes. */
	unsigned long long	   address;
	unsigned long long	   size;
	unsigned long		   index;
	unsigned char		   buf[0];
} co_monitor_ioctl_load_section_t;

/* interface for CO_MONITOR_IOCTL_LOAD_INITRD: */
typedef struct {
	co_manager_ioctl_monitor_t pc;
	unsigned long		   size;
	unsigned char		   buf[0];
} co_monitor_ioctl_load_initrd_t;

/* interface for CO_MONITOR_IOCTL_GET_CONSOLE: */
typedef struct {
	co_manager_ioctl_monitor_t pc;
	co_console_config_t        config;
} co_monitor_ioctl_get_console_t;

typedef struct {
	unsigned long	line;
	unsigned long	code;
	char		text[128];
} co_monitor_linux_bug_invocation_t;

/* interface for CO_MONITOR_IOCTL_GET_STATE: */
typedef struct {
	co_manager_ioctl_monitor_t 	  pc;
	unsigned long			  monitor_state;
	co_termination_reason_t		  termination_reason;
	co_monitor_linux_bug_invocation_t bug_info;
} co_monitor_ioctl_get_state_t;

/* interface for CO_MONITOR_IOCTL_RUN: */
typedef struct {
	co_manager_ioctl_monitor_t pc;
} co_monitor_ioctl_run_t;

typedef enum {
	CO_MONITOR_MESSAGE_TYPE_TERMINATED,
	CO_MONITOR_MESSAGE_TYPE_DEBUG_LINE,
	CO_MONITOR_MESSAGE_TYPE_TRACE_POINT,
} co_monitor_message_type_t;

typedef struct {
	co_monitor_message_type_t type;
	union {
		struct {
			co_termination_reason_t reason;
		} terminated;
	};
} co_daemon_message_t;

/* interface for CO_MONITOR_IOCTL_STATUS */
typedef struct co_monitor_ioctl_status {
	co_manager_ioctl_monitor_t pc;
} co_monitor_ioctl_status_t;

#ifdef CONFIG_COOPERATIVE_VIDEO
/* interface for CO_MONITOR_IOCTL_VIDEO_ATTACH/DETACH: */
typedef struct {
	co_manager_ioctl_monitor_t pc;
	int			   unit;
	void*			   address;
	void*			   handle;
} co_monitor_ioctl_video_t;
#endif

/***************** support kernel mode conet ***********************/
typedef enum {
	CO_CONET_BRIDGE,	/* bridge conet adapter to external */
	CO_CONET_NAT,		/* NAT conet traffic to external */
	CO_CONET_HOST		/* communicate with local host only */
} co_conet_protocol_t;

/* CO_MONITOR_IOCTL_CONET_BIND_ADAPTER */
typedef struct { /* for co_manager_ioctl_monitor_t extra_data */
	co_manager_ioctl_monitor_t pc;
	co_conet_protocol_t	   conet_proto;
	int		    	   conet_unit;
	int			   promisc_mode;
	char			   mac_address[6];
	char 			   netcfg_id[128]; /* null terminated */
} co_monitor_ioctl_conet_bind_adapter_t;

/* CO_MONITOR_IOCTL_CONET_UNBIND_ADAPTER */
typedef struct { /* for co_manager_ioctl_monitor_t extra_data */
	co_manager_ioctl_monitor_t pc;
	int			   conet_unit;
} co_monitor_ioctl_conet_unbind_adapter_t;

#endif
