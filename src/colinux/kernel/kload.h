/*
 * This source code is a part of coLinux source package.
 *
 * The code is licensed under the GPL. See the COPYING file at
 * the root directory.
 */

#ifndef __COLINUX_KERNEL_KLOAD_H__
#define __COLINUX_KERNEL_KLOAD_H__

#include <colinux/common/common.h>
#include <colinux/kernel/manager.h>
#include <colinux/arch/space.h>

/*
 * The guest's physical memory: a few contiguous host allocations, each one a
 * usable e820 range at its true host physical address. Block 0 is the image
 * plus the page-table region and is the only sizeable contiguous run required;
 * see kload.c for why the memory cannot be scattered pages.
 */
#define CO_KLOAD_RAM_BYTES	(128ULL << 20)
#define CO_KLOAD_MAX_BLOCKS	16

extern unsigned long long co_kload_phys_base(void);
extern int		  co_kload_range_count(void);
extern void		  co_kload_range(int i, unsigned long long* pa,
					 unsigned long long* usable,
					 unsigned long long* reserved);
extern co_rc_t		  co_kload_table_frame(co_manager_t* manager, co_pfn_t* pfn_out);
extern void*		  co_kload_frame_va(co_pfn_t pfn);

/*
 * Relocate the kernel's statically linked page tables to where the image
 * actually is, graft the host's own mappings into the top level, and return the
 * CR3 that puts the guest into the address space it was built to expect.
 * table_va[0] must be init_top_pgt.
 */
extern co_rc_t co_kload_adopt_kernel_tables(co_manager_t* manager,
					    const unsigned long long* table_va,
					    int table_count,
					    unsigned long long* cr3_out);

extern co_rc_t co_kload_begin(co_manager_t* manager, unsigned long long min_va,
			      unsigned long long max_va);
extern co_rc_t co_kload_chunk(co_manager_t* manager, unsigned long long va,
			      const unsigned char* data, unsigned long size, bool_t zero);
extern co_rc_t co_kload_verify(co_manager_t* manager, unsigned long long va,
			       unsigned long size, unsigned long long* sum_out);
extern co_rc_t co_kload_build_ram(co_manager_t* manager, unsigned long long ram_bytes,
				  unsigned long long text_va, unsigned long long end_va);
extern unsigned long co_kload_ram_pages(void);
extern void    co_kload_free(co_manager_t* manager);

extern co_arch_guest_space_t* co_kload_space(void);
extern unsigned long	      co_kload_pages(void);
extern unsigned long	      co_kload_chunks(void);

#endif
