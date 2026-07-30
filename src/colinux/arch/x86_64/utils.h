/*
 * This source code is a part of coLinux source package.
 *
 * The code is licensed under the GPL. See the COPYING file at
 * the root directory.
 */

#ifndef __COLINUX_ARCH_X86_64_UTILS_H__
#define __COLINUX_ARCH_X86_64_UTILS_H__

#include <colinux/common/common.h>

/*
 * There is no co_is_pae_enabled() counterpart here. PAE is not optional in long
 * mode -- CR4.PAE is necessarily set -- so the question the i386 code asks in
 * order to choose between two variants of the world switch has no meaning.
 */

extern unsigned long long co_get_cr0(void);
extern unsigned long long co_get_cr2(void);
extern unsigned long long co_get_cr3(void);
extern unsigned long long co_get_cr4(void);
extern unsigned long long co_get_dr0(void);
extern unsigned long long co_get_dr1(void);
extern unsigned long long co_get_dr2(void);
extern unsigned long long co_get_dr3(void);
extern unsigned long long co_get_dr6(void);
extern unsigned long long co_get_dr7(void);

extern unsigned long long co_read_msr(unsigned long msr);
extern void co_write_msr(unsigned long msr, unsigned long long value);

#endif
