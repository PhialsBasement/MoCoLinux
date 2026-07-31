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
 * The guest's physical memory: one contiguous host allocation, with guest
 * physical N at host physical base + N. See kload.c for why it cannot be
 * scattered pages.
 */
#define CO_KLOAD_RAM_BYTES	(128ULL << 20)

extern co_pa_t		  co_kload_block_pa(void);
extern unsigned long long co_kload_usable_bytes(void);
extern co_rc_t		  co_kload_table_frame(co_manager_t* manager, co_pfn_t* pfn_out);
extern void*		  co_kload_frame_va(co_pfn_t pfn);

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
