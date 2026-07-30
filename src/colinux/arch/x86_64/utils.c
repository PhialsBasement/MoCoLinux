/*
 * This source code is a part of coLinux source package.
 *
 * The code is licensed under the GPL. See the COPYING file at
 * the root directory.
 *
 */

#include <colinux/common/common.h>

#include "utils.h"

/*
 * "=r" on an unsigned long long picks a 64-bit register, so these assemble as
 * mov %crN, %rax rather than the 32-bit form. In long mode there is no 32-bit
 * encoding of a control-register move anyway.
 */

unsigned long long co_get_cr0(void)
{
	unsigned long long reg = 0;
	asm("mov %%cr0, %0" : "=r"(reg));
	return reg;
}

unsigned long long co_get_cr2(void)
{
	unsigned long long reg = 0;
	asm("mov %%cr2, %0" : "=r"(reg));
	return reg;
}

unsigned long long co_get_cr3(void)
{
	unsigned long long reg = 0;
	asm("mov %%cr3, %0" : "=r"(reg));
	return reg;
}

unsigned long long co_get_cr4(void)
{
	unsigned long long reg = 0;
	asm("mov %%cr4, %0" : "=r"(reg));
	return reg;
}

unsigned long long co_get_dr0(void)
{
	unsigned long long reg = 0;
	asm("mov %%dr0, %0" : "=r"(reg));
	return reg;
}

unsigned long long co_get_dr1(void)
{
	unsigned long long reg = 0;
	asm("mov %%dr1, %0" : "=r"(reg));
	return reg;
}

unsigned long long co_get_dr2(void)
{
	unsigned long long reg = 0;
	asm("mov %%dr2, %0" : "=r"(reg));
	return reg;
}

unsigned long long co_get_dr3(void)
{
	unsigned long long reg = 0;
	asm("mov %%dr3, %0" : "=r"(reg));
	return reg;
}

unsigned long long co_get_dr6(void)
{
	unsigned long long reg = 0;
	asm("mov %%dr6, %0" : "=r"(reg));
	return reg;
}

unsigned long long co_get_dr7(void)
{
	unsigned long long reg = 0;
	asm("mov %%dr7, %0" : "=r"(reg));
	return reg;
}

/*
 * rdmsr/wrmsr always use EDX:EAX, in both 32-bit and 64-bit mode -- there is no
 * 64-bit-register form -- so the halves have to be joined and split by hand.
 * Note this is exactly what the i386 passage code gets wrong: it keeps only EAX
 * on the way out and zeroes EDX on the way back in, which would silently
 * truncate every MSR on a 64-bit host.
 */

unsigned long long co_read_msr(unsigned long msr)
{
	unsigned int low, high;

	asm volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(msr));

	return ((unsigned long long)high << 32) | low;
}

void co_write_msr(unsigned long msr, unsigned long long value)
{
	unsigned int low  = (unsigned int)value;
	unsigned int high = (unsigned int)(value >> 32);

	asm volatile("wrmsr" : : "c"(msr), "a"(low), "d"(high));
}
