/*
 * This source code is a part of coLinux source package.
 *
 * The code is licensed under the GPL. See the COPYING file at
 * the root directory.
 */

#ifndef __COLINUX_ARCH_X86_64_MANAGER_H__
#define __COLINUX_ARCH_X86_64_MANAGER_H__

#include <colinux/common/common.h>
#include <colinux/kernel/manager.h>
#include <colinux/arch/manager.h>
#include <colinux/os/alloc.h>

#include "cpuid.h"

struct co_archdep_manager {
	unsigned int caps[CO_CPUID_CAPS_COUNT];
};

/*
 * No antinx state here. The i386 port walks the host's live page tables to clear
 * the NX bit on the passage page, because the page comes from an allocation
 * Windows may mark non-executable. On x64 that walk becomes four levels deep and
 * PatchGuard objects to page-table tampering, so the passage page has to come
 * from an allocation that is executable to begin with. See doc/porting-x86_64
 * section 4.6.
 */
struct co_archdep_monitor {
	int unused;
};

#endif
