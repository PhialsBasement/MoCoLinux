/*
 * This source code is a part of coLinux source package.
 *
 * The code is licensed under the GPL. See the COPYING file at
 * the root directory.
 */

/*
 * CO_MANAGER_IOCTL_PROBE_VA is an x86-64 porting aid: it answers whether the
 * host has anything mapped over the address window the guest wants, which
 * determines whether the passage page can live at the same virtual address on
 * both sides. The i386 port already knows its answer -- the guest window at
 * 0xffc00000 is fixed and the relocation path exists -- so there is nothing to
 * discover here, and a 2-or-3-level walker would have to be written to find it
 * out. Report unsupported rather than pretend.
 */

#include <colinux/common/common.h>
#include <colinux/kernel/manager.h>
#include <colinux/arch/probe.h>

void co_arch_probe_va(co_manager_t *manager, co_manager_ioctl_probe_va_t *params)
{
	int level;

	params->supported     = PFALSE;
	params->present       = PFALSE;
	params->large_page    = PFALSE;
	params->levels_walked = 0;
	params->cr3           = 0;
	for (level = 0; level < CO_PROBE_VA_LEVELS; level++)
		params->entry[level] = 0;

	params->rc = CO_RC(OK);
}
