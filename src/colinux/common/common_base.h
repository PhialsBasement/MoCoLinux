/*
 * This source code is a part of coLinux source package.
 *
 * Dan Aloni <da-x@colinux.org>, 2003 (c)
 *
 * The code is licensed under the GPL. See the COPYING file at
 * the root directory.
 */

#ifndef __COLINUX_LINUX_COMMON_BASE_H__
#define __COLINUX_LINUX_COMMON_BASE_H__

typedef int bool_t;

#define PFALSE  0
#define PTRUE   1

#define PACKED_STRUCT __attribute__((packed))

#define CO_MAX_MONITORS                   64
#define CO_LINUX_PERIPHERY_API_VERSION    30

#define CO_ERRORS_X_MACRO			\
	X(ERROR)				\
	X(PAE_ENABLED)	         		\
	X(VERSION_MISMATCHED)			\
	X(OUT_OF_MEMORY)			\
	X(OUT_OF_PAGES)				\
	X(NOT_FOUND)				\
	X(CANT_OPEN_VMLINUX_FILE)		\
	X(CANT_STAT_VMLINUX_FILE)		\
	X(ERROR_READING_VMLINUX_FILE)		\
	X(ERROR_INSTALLING_DRIVER)		\
	X(ERROR_REMOVING_DRIVER)		\
	X(ERROR_STARTING_DRIVER)		\
	X(ERROR_STOPPING_DRIVER)		\
	X(ERROR_ACCESSING_DRIVER)		\
	X(TRANSFER_OFF_BOUNDS)			\
	X(MONITOR_NOT_LOADED)			\
	X(BROKEN_PIPE)				\
	X(TIMEOUT)				\
	X(ACCESS_DENIED)			\
	X(COMPILER_MISMATCHED)			\
	X(INVALID_PARAMETER)			\
	X(HOSTMEM_USE_LIMIT_REACHED)		\
	X(INSTANCE_TERMINATED)			\

#define X(name) CO_RC_ERROR_##name,
typedef enum {
    CO_RC_NO_ERROR=0,
    CO_ERRORS_X_MACRO
} co_error_value_t;
#undef X

#define X(name) CO_RC_##name = -CO_RC_ERROR_##name,
typedef enum {
    CO_RC_OK                         = 0,
    CO_ERRORS_X_MACRO
} co_rc_value_t;
#undef X

typedef long co_rc_t;

#include "debug.h"

#ifndef COLINUX_FILE_ID
#define COLINUX_FILE_ID           -1
#endif

/*
 * co_rc_t is comprised of:
 *
 * 31                               0
 *   --------------------------------
 *                         eeeeeeeeee   10
 *              lllllllllll             11
 *    iiiiiiiiii                        10
 *
 *  i - file id
 *  l - line number
 *  e - error code
 *
 */

#define CO_BITS_OFFSET_ERROR_CODE   0
#define CO_BITS_COUNT_ERROR_CODE    10
#define CO_BITS_OFFSET_LINE_NUM     (CO_BITS_OFFSET_ERROR_CODE + CO_BITS_COUNT_ERROR_CODE)
#define CO_BITS_COUNT_LINE_NUM      11
#define CO_BITS_OFFSET_FILE_ID      (CO_BITS_OFFSET_LINE_NUM + CO_BITS_COUNT_LINE_NUM)
#define CO_BITS_COUNT_FILE_ID       10
#define CO_BITS_OFFSET_NEG          (CO_BITS_OFFSET_FILE_ID + CO_BITS_COUNT_FILE_ID)
#define CO_BITS_COUNT_NEG           1

#define CO_BITS_BUILD(type, value) \
	((value & ((1 << CO_BITS_COUNT_##type) - 1)) << CO_BITS_OFFSET_##type)

#define CO_BITS_GET(type, value) \
	((value >> CO_BITS_OFFSET_##type) & ((1 << CO_BITS_COUNT_##type) - 1))

#define CO_RC_NEG_WRAPPER(name)						\
	(CO_BITS_BUILD(NEG, name < 0 ? 1 : 0) |				\
	 CO_BITS_BUILD(ERROR_CODE, name < 0 ? -name : name) |		\
	 CO_BITS_BUILD(LINE_NUM, __LINE__) |				\
	 CO_BITS_BUILD(FILE_ID, COLINUX_FILE_ID) |			\
	 0)

#define CO_RC_GET_CODE(value)     ((value < 0) ? (-CO_BITS_GET(ERROR_CODE, value)) : (CO_BITS_GET(ERROR_CODE, value)))
#define CO_RC_GET_LINE(value)     CO_BITS_GET(LINE_NUM, value)
#define CO_RC_GET_FILE_ID(value)  CO_BITS_GET(FILE_ID, value)

#ifndef DEBUG_CO_RC
#define CO_RC(name)      CO_RC_NEG_WRAPPER(CO_RC_##name)
#define CO_OK(rc)        (rc >= 0)
#else
#define CO_RC(name)      ({ \
                        co_rc_t __rc__ = CO_RC_##name; \
                        co_debug("%s():%d: CO_RC(%d)", __FUNCTION__, __LINE__, __rc__); \
                        __rc__; \
})
#define CO_OK(rc)        ({ \
			co_rc_t __rc__ = rc; \
                        co_debug("%s():%d: CO_OK(%d)", __FUNCTION__, __LINE__, __rc__); \
                        __rc__ >= 0; \
})
#endif

/*
 * Defines a LINUX instance. There are CO_MAX_COLINUXS of these
 */
typedef unsigned int co_id_t;

#define CO_INVALID_ID ((co_id_t)-1)

/*
 * The definition of a pathname on the host.
 */
typedef char co_pathname_t[0x100];

/*
 * The maximum number of sections that we can load off the core image.
 */
#define CO_MAX_CORE_SECTIONS                        0x100

#include <stdarg.h>

extern int co_snprintf(char *str, long n, const char *format, ...)
	__attribute__ ((format (printf, 3, 4)));
extern int co_vsnprintf(char *str, long nmax, const char *format, va_list ap)
	__attribute__ ((format (printf, 3, 0)));
extern void co_rc_format_error(co_rc_t rc, char *buf, int size);

#endif
