/*
 * This source code is a part of coLinux source package.
 *
 * The code is licensed under the GPL. See the COPYING file at
 * the root directory.
 */

#include <colinux/common/common.h>

#include "defs.h"
#include "apic.h"

co_rc_t co_manager_arch_init_apic(co_archdep_manager_t manager)
{
	co_debug("arch APIC initialization");

	/*
	 * Note the shift. The i386 version tests caps[0] & CO_ARCH_X86_FEATURE_APIC,
	 * i.e. & 9, which happens to pass on any CPU because bit 0 (FPU) is set --
	 * so it has never actually checked for an APIC. Compare the correct
	 * 1 << CO_ARCH_X86_FEATURE_SEP test in the i386 passage code.
	 */
	if (!(manager->caps[CO_CPUID_CAPS_1_EDX] & (1 << CO_ARCH_X86_FEATURE_APIC))) {
		co_debug_error("no APIC support");
		return CO_RC(ERROR);
	}

	co_debug("found APIC support");

	/*
	 * Still nothing to do, as on i386. Whether coLinux needs to touch the APIC
	 * at all remains an open question there and is no different here.
	 */

	return CO_RC(OK);
}
