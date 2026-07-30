/*
 * This source code is a part of coLinux source package.
 *
 * The code is licensed under the GPL. See the COPYING file at
 * the root directory.
 */

#ifndef __COLINUX_ARCH_X86_64_CPUID_H__
#define __COLINUX_ARCH_X86_64_CPUID_H__

#include <colinux/common/common.h>

typedef union {
	struct {
		unsigned int eax, ebx, ecx, edx;
	};
	struct {
		unsigned int highest_op;
		char id_string[12];
	};
} cpuid_t;

/*
 * Indices into the capability array. The i386 port keeps two words and hands
 * them around as caps[0] and caps[1] with the meanings implied by position;
 * these are named because two more are needed here.
 */
enum {
	CO_CPUID_CAPS_1_EDX = 0,	/* leaf 1, EDX  */
	CO_CPUID_CAPS_1_ECX,		/* leaf 1, ECX  */
	CO_CPUID_CAPS_EXT_EDX,		/* leaf 0x80000001, EDX: NX, LM */
	CO_CPUID_CAPS_7_EBX,		/* leaf 7 subleaf 0, EBX: SMEP, SMAP */
	CO_CPUID_CAPS_COUNT
};

/*
 * No co_x86_has_cpuid(). The i386 port probes for CPUID by trying to toggle
 * EFLAGS.ID, which is meaningless on x86-64 -- CPUID is architectural, and any
 * CPU capable of long mode has it. The probe's asm would not assemble here
 * either, since it round-trips flags through 32-bit pushes.
 */
void co_x86_get_cpuid(unsigned int op, cpuid_t *cpuid);
void co_x86_get_cpuid_count(unsigned int op, unsigned int count, cpuid_t *cpuid);
co_rc_t co_x86_get_cpuid_capabilities(unsigned int *caps);

#endif
