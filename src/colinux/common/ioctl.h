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
	unsigned long		   address;
	unsigned long		   size;
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
