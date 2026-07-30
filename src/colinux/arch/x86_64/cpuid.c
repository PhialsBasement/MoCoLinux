/*
 * This source code is a part of coLinux source package.
 *
 * The code is licensed under the GPL. See the COPYING file at
 * the root directory.
 */

#include "cpuid.h"
#include "defs.h"

void co_x86_get_cpuid_count(unsigned int op, unsigned int count, cpuid_t *cpuid)
{
	asm("cpuid"
	    : "=a" (cpuid->eax),
	      "=b" (cpuid->ebx),
	      "=c" (cpuid->ecx),
	      "=d" (cpuid->edx)
	    : "0" (op), "2" (count));
}

void co_x86_get_cpuid(unsigned int op, cpuid_t *cpuid)
{
	co_x86_get_cpuid_count(op, 0, cpuid);
}

co_rc_t co_x86_get_cpuid_capabilities(unsigned int *caps)
{
	cpuid_t cpuid;
	unsigned int highest_op, highest_ext;

	co_x86_get_cpuid(0, &cpuid);
	highest_op = cpuid.highest_op;
	if (highest_op < 0x00000001)
		return CO_RC(ERROR);

	co_x86_get_cpuid(0x00000001, &cpuid);
	caps[CO_CPUID_CAPS_1_EDX] = cpuid.edx;
	caps[CO_CPUID_CAPS_1_ECX] = cpuid.ecx;

	co_x86_get_cpuid(0x80000000, &cpuid);
	highest_ext = cpuid.eax;
	if ((highest_ext & 0xffff0000) == 0x80000000 && highest_ext >= 0x80000001) {
		co_x86_get_cpuid(0x80000001, &cpuid);
		caps[CO_CPUID_CAPS_EXT_EDX] = cpuid.edx;
	}

	if (highest_op >= 0x00000007) {
		co_x86_get_cpuid_count(0x00000007, 0, &cpuid);
		caps[CO_CPUID_CAPS_7_EBX] = cpuid.ebx;
	}

	/*
	 * Long mode is not optional here. If the CPU cannot report it, either the
	 * driver is running somewhere impossible or the query itself is wrong;
	 * either way, refuse rather than proceed on a guess.
	 */
	if (!(caps[CO_CPUID_CAPS_EXT_EDX] & (1 << CO_ARCH_X86_FEATURE_LM))) {
		co_debug_error("CPU does not report long mode support");
		return CO_RC(ERROR);
	}

	return CO_RC(OK);
}
