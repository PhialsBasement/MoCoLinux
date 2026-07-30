/*
 * This source code is a part of coLinux source package.
 *
 * The code is licensed under the GPL. See the COPYING file at
 * the root directory.
 */

#ifndef __COLINUX_ARCH_STATE_H__
#define __COLINUX_ARCH_STATE_H__

#include <colinux/common/common.h>
#include <colinux/kernel/manager.h>

/*
 * Capture the current CPU state. Reads only -- writes nothing back -- so it is
 * safe to call at any time. The save half of the world switch.
 */
extern void co_arch_save_state(co_arch_state_stack_t* state);

/* Put a previously captured state back. Writes control registers, descriptor
 * tables and MSRs with interrupts off. */
extern void co_arch_restore_state(co_arch_state_stack_t* state);

/* Save then immediately restore, never changing address space. Returning at all
 * is the result. */
extern void co_arch_test_save_restore(co_arch_state_stack_t* state);

#endif
