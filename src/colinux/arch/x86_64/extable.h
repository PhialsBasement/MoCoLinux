/*
 * This source code is a part of coLinux source package.
 *
 * The code is licensed under the GPL. See the COPYING file at
 * the root directory.
 */

#ifndef __COLINUX_ARCH_X86_64_EXTABLE_H__
#define __COLINUX_ARCH_X86_64_EXTABLE_H__

#include <colinux/common/common.h>
#include <colinux/kernel/manager.h>

/*
 * The guest kernel's exception table, applied by the host.
 *
 * Linux provokes faults on purpose. A segment load with a value that may be
 * garbage, an rdmsr of a register this processor may not have, a copy to a
 * user address that may not be mapped -- each is written as an instruction
 * plus an entry in __ex_table saying where to resume and what to leave in a
 * register. The kernel's own #GP and #PF handlers consult that table and carry
 * on; the fault is expected, and recovering from it is the point.
 *
 * A cooperative guest runs on the host's IDT, so those handlers never run and
 * an ordinary recoverable fault ends the boot instead. The host therefore has
 * to honour the contract itself: look the faulting address up in the guest's
 * own table and do exactly what the kernel's handler would have done.
 *
 * This is the same arrangement already used for WARN_ON: the kernel encodes
 * what it wants in a table, and because the code that reads the table does not
 * run here, the host reads it instead.
 */

/* Load the guest's table. Both addresses are guest virtual. */
extern co_rc_t co_arch_extable_load(co_manager_t* manager,
				    unsigned long long start_va,
				    unsigned long long stop_va);

extern void co_arch_extable_free(void);

/* How many entries were loaded, for reporting. */
extern int co_arch_extable_count(void);

/*
 * Try to fix a fault up the way the guest's own handler would.
 *
 * frame is the stub's saved register block. On success the frame's RIP, and
 * whichever registers the entry names, have been updated and the guest can be
 * resumed. Returns PFALSE if there is no entry for this address, or if there
 * is one whose type this does not implement -- and says which, because a fault
 * silently treated as unfixable is indistinguishable from a real one.
 */
extern bool_t co_arch_extable_fixup(unsigned long long* frame,
				    unsigned long long vector,
				    int* type_out);

#endif
