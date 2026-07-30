#ifndef __COLINUX_OS_WINNT_MEMORY_H__
#define __COLINUX_OS_WINNT_MEMORY_H__

/*
 * string.h rather than memory.h: mingw-w64's memory.h declares only memchr,
 * memcmp, memcpy and memset, and common/libc.c also wraps memmove and strstr.
 */
#include <string.h>

#endif
