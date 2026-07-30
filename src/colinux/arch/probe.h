/*
 * This source code is a part of coLinux source package.
 *
 * The code is licensed under the GPL. See the COPYING file at
 * the root directory.
 */

#ifndef __COLINUX_ARCH_PROBE_H__
#define __COLINUX_ARCH_PROBE_H__

#include <colinux/common/common.h>
#include <colinux/common/ioctl.h>
#include <colinux/kernel/manager.h>

/*
 * Walk the host's page tables for params->va and fill in what is mapped at each
 * level. Read-only. Sets params->supported to PFALSE on architectures where this
 * is not implemented.
 */
extern void co_arch_probe_va(co_manager_t *manager,
			     co_manager_ioctl_probe_va_t *params);

#endif
