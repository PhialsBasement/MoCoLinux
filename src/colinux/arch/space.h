/*
 * This source code is a part of coLinux source package.
 *
 * The code is licensed under the GPL. See the COPYING file at
 * the root directory.
 */

#ifndef __COLINUX_ARCH_SPACE_H__
#define __COLINUX_ARCH_SPACE_H__

#include <colinux/common/common.h>
#include <colinux/kernel/manager.h>
#include <colinux/os/kernel/misc.h>

/*
 * Building a guest address space.
 *
 * The switch tests so far have used one hand-rolled page table that maps a
 * single 15-page allocation, which is all they needed. A guest that runs Linux
 * needs an address space in the ordinary sense: a kernel image near the top of
 * the canonical half, a linear map of pseudo-physical RAM, and the passage page,
 * none of them contiguous and none at addresses a fixed two-level builder can
 * reach.
 *
 * So this is the general primitive: create a space, map (va -> pa) into it one
 * page at a time and let the intermediate tables come into being as needed, and
 * look an address back up to see what the hardware would find.
 *
 * The lookup half exists specifically so a space can be checked before it is
 * ever loaded into CR3. A wrong page table is not a wrong answer -- it is a
 * triple fault with no bugcheck and nothing to read afterwards, so the only
 * useful time to find the mistake is while the space is still just data.
 */

typedef struct co_arch_guest_space co_arch_guest_space_t;

extern co_rc_t co_arch_guest_space_create(co_manager_t* manager,
					  co_arch_guest_space_t** space_out);

/*
 * Frees the space and every page table in it. Pages that were *mapped* into the
 * space belong to whoever supplied them and are left alone.
 */
extern void co_arch_guest_space_destroy(co_manager_t* manager,
					co_arch_guest_space_t* space);

extern co_rc_t co_arch_guest_map(co_manager_t* manager,
				 co_arch_guest_space_t* space,
				 unsigned long long va,
				 co_pa_t pa,
				 unsigned long long flags);

/*
 * What the hardware would find at va. On success *pa_out is the mapped page.
 * On a miss *level_out says which level was absent, counting from the top
 * (0 = PML4, 3 = PT), which is the difference between "nothing here at all" and
 * "the tables exist but this page is not present".
 */
extern co_rc_t co_arch_guest_lookup(co_manager_t* manager,
				    co_arch_guest_space_t* space,
				    unsigned long long va,
				    co_pa_t* pa_out,
				    int* level_out);

/* The value that would go into CR3. */
extern co_pa_t co_arch_guest_space_root(co_arch_guest_space_t* space);

/* How many page-table pages the space currently owns. */
extern unsigned long co_arch_guest_space_tables(co_arch_guest_space_t* space);

/*
 * Build a representative guest address space and verify it by reading it back,
 * without ever loading it into CR3.
 */
typedef struct {
	int		   supported;
	int		   succeeded;
	unsigned long long root;
	unsigned long	   mapped;
	unsigned long	   verified;
	unsigned long	   mismatched;
	unsigned long	   tables;
	unsigned long long first_bad_va;
	unsigned long long first_bad_expect;
	unsigned long long first_bad_got;
	int		   unmapped_reported;
	int		   unmapped_level;
} co_arch_space_test_t;

extern co_rc_t co_arch_test_space(co_manager_t* manager, co_arch_space_test_t* out);

#endif
