/*
 * This source code is a part of coLinux source package.
 *
 * The code is licensed under the GPL. See the COPYING file at
 * the root directory.
 */

/*
 * An x86-64 bring-up aid. i386's passage code has been changing CR3 and
 * continuing to execute since 2004; there is nothing to establish here.
 */

#include <colinux/common/common.h>
#include <colinux/common/libc.h>
#include <colinux/arch/switch.h>

co_rc_t co_arch_test_switch(co_manager_t* manager, co_arch_switch_test_t* out)
{
	co_memset(out, 0, sizeof(*out));
	out->supported = PFALSE;
	return CO_RC(OK);
}

co_rc_t co_arch_test_roundtrip(co_manager_t* manager, co_arch_switch_test_t* out,
			       bool_t provoke_fault)
{
	co_memset(out, 0, sizeof(*out));
	out->supported = PFALSE;
	return CO_RC(OK);
}

co_rc_t co_arch_test_resume(co_manager_t* manager, co_arch_switch_test_t* out,
			    int iterations)
{
	co_memset(out, 0, sizeof(*out));
	out->supported = PFALSE;
	return CO_RC(OK);
}
