/*
 * This source code is a part of coLinux source package.
 *
 * The code is licensed under the GPL. See the COPYING file at
 * the root directory.
 */

/*
 * The state-capture helper is an x86-64 bring-up aid: it exists to check the
 * long-mode state layout and to see what Windows' actual state looks like before
 * the restore half is written. i386's passage code already saves its own state
 * inline in assembly and has done for twenty years, so there is nothing to learn
 * here and no reason to duplicate it.
 */

#include <colinux/common/common.h>
#include <colinux/common/libc.h>
#include <colinux/arch/state.h>

void co_arch_save_state(co_arch_state_stack_t* state)
{
	co_memset(state, 0, sizeof(*state));
}
