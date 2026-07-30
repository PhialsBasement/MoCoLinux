/*
 * This source code is a part of coLinux source package.
 *
 * The code is licensed under the GPL. See the COPYING file at
 * the root directory.
 */

/*
 * An x86-64 bring-up aid. The i386 port builds its guest address spaces inside
 * the passage page with fixed two- and three-level builders and has done since
 * 2004; there is no general primitive to provide here.
 */

#include <colinux/common/common.h>
#include <colinux/common/libc.h>
#include <colinux/arch/space.h>

co_rc_t co_arch_guest_space_create(co_manager_t* manager,
				   co_arch_guest_space_t** space_out)
{
	*space_out = NULL;
	return CO_RC(ERROR);
}

void co_arch_guest_space_destroy(co_manager_t* manager, co_arch_guest_space_t* space)
{
}

co_rc_t co_arch_guest_map(co_manager_t* manager, co_arch_guest_space_t* space,
			  unsigned long long va, co_pa_t pa, unsigned long long flags)
{
	return CO_RC(ERROR);
}

co_rc_t co_arch_guest_lookup(co_manager_t* manager, co_arch_guest_space_t* space,
			     unsigned long long va, co_pa_t* pa_out, int* level_out)
{
	*pa_out    = 0;
	*level_out = -1;
	return CO_RC(ERROR);
}

co_pa_t co_arch_guest_space_root(co_arch_guest_space_t* space)
{
	return 0;
}

unsigned long co_arch_guest_space_tables(co_arch_guest_space_t* space)
{
	return 0;
}

co_rc_t co_arch_test_space(co_manager_t* manager, co_arch_space_test_t* out)
{
	co_memset(out, 0, sizeof(*out));
	out->supported = PFALSE;
	return CO_RC(OK);
}
