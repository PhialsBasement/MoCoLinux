/*
 * This source code is a part of coLinux source package.
 *
 * Dan Aloni <da-x@colinux.org>, 2003 (c)
 *
 * The code is licensed under the GPL. See the COPYING file at
 * the root directory.
 *
 */

#ifndef __NESTED_WINNT_DDK_H__
#define __NESTED_WINNT_DDK_H__

#include <ddk/ntddk.h>
/* Zw{QueryDirectoryFile,SetSecurityObject} and friends, formerly ddk/ntapi.h */
#include <ddk/ntifs.h>

/*
 * Deliberately not including ddk/ntstrsafe.h here: it defines every Rtl string
 * helper with external linkage in each translation unit that sees it, so
 * pulling it into this shared header makes them multiply defined at link time.
 * Only the separate tap-win32 driver uses those functions; it should include
 * the header itself.
 */

/*
 * DDKAPI was the old w32api spelling of the __stdcall convention used by DDK
 * callbacks. mingw-w64 does not define it; NTAPI is the same thing.
 */
#ifndef DDKAPI
#define DDKAPI NTAPI
#endif

/*
 * There is no libc in kernel mode. ntoskrnl exports _strnicmp, the equivalent of
 * strncasecmp, but the mingw-w64 DDK headers do not declare it.
 */
int __cdecl _strnicmp(const char *s1, const char *s2, size_t n);

/*
 * mingw-w64's DDK has no equivalent of the old w32api ddk/ntapi.h, and does not
 * declare NtQuerySystemInformation or SYSTEM_BASIC_INFORMATION even though
 * ntoskrnl exports the former. Declare the minimum needed by
 * co_os_physical_memory_pages().
 *
 * Layout is the long-standing one also described by winternl.h, where the
 * leading six ULONGs appear as Reserved1[24]. Only NumberOfPhysicalPages, at
 * offset 12, is read.
 */
#ifndef SystemBasicInformation
#define SystemBasicInformation 0
#endif

typedef struct _CO_SYSTEM_BASIC_INFORMATION {
	ULONG		Reserved;
	ULONG		TimerResolution;
	ULONG		PageSize;
	ULONG		NumberOfPhysicalPages;
	ULONG		LowestPhysicalPageNumber;
	ULONG		HighestPhysicalPageNumber;
	ULONG		AllocationGranularity;
	ULONG_PTR	MinimumUserModeAddress;
	ULONG_PTR	MaximumUserModeAddress;
	ULONG_PTR	ActiveProcessorsAffinityMask;
	CCHAR		NumberOfProcessors;
} SYSTEM_BASIC_INFORMATION, *PSYSTEM_BASIC_INFORMATION;

NTSYSAPI NTSTATUS NTAPI NtQuerySystemInformation(
	ULONG	SystemInformationClass,
	PVOID	SystemInformation,
	ULONG	SystemInformationLength,
	PULONG	ReturnLength);

/*
 * The DDK name for this structure is FILE_BOTH_DIRECTORY_INFORMATION; mingw-w64's
 * ntifs.h declares the identical layout as FILE_BOTH_DIR_INFORMATION.
 */
typedef FILE_BOTH_DIR_INFORMATION FILE_BOTH_DIRECTORY_INFORMATION;
typedef FILE_BOTH_DIR_INFORMATION *PFILE_BOTH_DIRECTORY_INFORMATION;

#endif
