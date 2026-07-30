/*
 * This source code is a part of coLinux source package.
 *
 * The code is licensed under the GPL. See the COPYING file at
 * the root directory.
 */

#ifndef __COLINUX_ARCH_SWITCH_H__
#define __COLINUX_ARCH_SWITCH_H__

#include <colinux/common/common.h>
#include <colinux/kernel/manager.h>

/*
 * Result of changing CR3 into a minimal guest address space and continuing to
 * execute there. See arch/x86_64/switch.c.
 */
typedef struct {
	int		   supported;
	int		   succeeded;
	unsigned long long passage_va;
	unsigned long long passage_pa;
	unsigned long long code_va;
	unsigned long long host_cr3;
	unsigned long long guest_cr3;
	unsigned long long expected;
	unsigned long long observed;
	unsigned long	   code_size;
} co_arch_switch_test_t;

extern co_rc_t co_arch_test_switch(co_manager_t* manager, co_arch_switch_test_t* out);

#endif
