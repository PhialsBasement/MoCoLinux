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
 * Host allocations are only virtually contiguous.  The p2m table records the
 * machine PFN of every 4 KB guest page, so physical fragmentation is invisible
 * to Linux and never drives an MmAllocateContiguousMemory search.
 *
 * 32 MB keeps allocation latency and bookkeeping bounded. The pages inside a
 * block are physically scattered; only its kernel virtual address is linear.
 * Allocation falls back as far as one page if nonpaged-pool virtual space is
 * itself fragmented. 16384 entries let a 128 GB guest average 8 MB per block,
 * four times the metadata headroom of an unfragmented 32 MB layout.
 */
#define CO_KLOAD_CHUNK_BYTES	(32ULL << 20)
#define CO_KLOAD_MAX_BLOCKS	16384

/* Keep these in lockstep with arch/x86/include/asm/cooperative.h in the guest. */
#define CO_KLOAD_M2P_VA		0xffffe99fc0000000ULL
#define CO_KLOAD_P2M_VA		0xffffe99f80000000ULL
#define CO_KLOAD_TRANSLATION_WINDOW_BYTES (1ULL << 30)
#define CO_KLOAD_PFN_MASK	0x000ffffffffff000ULL

/*
 * The blocks themselves, for the KMAP path, plus the reference count that
 * keeps teardown from freeing pages a user mapping still names. See the
 * comment on kload_user_maps in kload.c.
 */
extern int	 co_kload_block_count(void);
extern co_rc_t	 co_kload_block(int i, void** va, unsigned long long* pa,
				unsigned long long* bytes);
/* The complete allocation block containing one pseudo-physical byte. */
extern co_rc_t	 co_kload_block_for_pa(unsigned long long query_pa, void** va,
				       unsigned long long* block_pa,
				       unsigned long long* block_bytes);
/* Reserve the finished RAM image for a KMAP caller. */
extern bool_t	 co_kload_user_map_try_get(void);
extern void	 co_kload_user_map_put(co_manager_t* manager);

extern unsigned long long co_kload_phys_base(void);
extern int		  co_kload_range_count(void);
extern void		  co_kload_range(int i, unsigned long long* pa,
					 unsigned long long* usable,
					 unsigned long long* reserved);
extern co_rc_t		  co_kload_table_frame(co_manager_t* manager, co_pfn_t* pfn_out);
/* Machine PFN, as found in a hardware page-table entry. */
extern void*		  co_kload_frame_va(co_pfn_t pfn);
/* Pseudo PFN, as used by Linux, e820 and cooperative device requests. */
extern void*		  co_kload_pseudo_frame_va(co_pfn_t pfn);
extern co_rc_t		  co_kload_pseudo_to_machine(co_pa_t pseudo,
					     co_pa_t* machine);
/*
 * Make host memory appear in the guest's physical address space.
 *
 * For Venus, where the guest must write into memory the host's GPU reads. The
 * caller owns the machine frames and must keep them resident for as long as
 * the window is mapped -- locked with an MDL, in practice, since the guest
 * will be handed page-table entries naming them.
 *
 * Nothing is reserved in advance: address space is taken here and returned by
 * the unmap, so OUT_OF_MEMORY is an ordinary answer rather than a failure.
 * Frame zero is rejected, being indistinguishable from an unmapped page.
 */
extern co_rc_t		  co_kload_window_map(const co_pfn_t* mfns,
					      unsigned long count,
					      co_pa_t* pseudo_out);
extern void		  co_kload_window_unmap(co_pa_t pseudo,
						unsigned long count);
extern unsigned long	  co_kload_p2m_pages(void);
extern unsigned long long co_kload_m2p_mask(void);
/*
 * One guest virtual address resolved once to a host pointer, for readers
 * that cannot afford a walk per access. Valid only while the guest is up --
 * see the implementation for the lifetime rule.
 */
extern co_rc_t		  co_kload_virt_to_phys(co_manager_t* manager,
					        unsigned long long va, co_pa_t* pa);
extern void*		  co_kload_host_ptr(co_manager_t* manager,
				      unsigned long long va);

/*
 * Guest memory through the root the guest was running on, rather than the
 * host's load-time space. Needed for anything on cpu_entry_area or a vmalloc'd
 * task stack, which the load-time root does not map at all.
 */
extern co_rc_t		  co_kload_read_cr3(co_manager_t* manager,
					    unsigned long long cr3,
					    unsigned long long va,
					    unsigned char* buf, unsigned long size);
extern co_rc_t		  co_kload_write_cr3(co_manager_t* manager,
					     unsigned long long cr3,
					     unsigned long long va,
					     const unsigned char* buf,
					     unsigned long size);

/*
 * Translate the usable static fixmap parent, graft the host's 4 KB image and
 * direct-map subtrees into init_top_pgt, and return its machine CR3.
 * table_va[0] must be init_top_pgt.
 */
extern co_rc_t co_kload_adopt_kernel_tables(co_manager_t* manager,
					    const unsigned long long* table_va,
					    int table_count,
					    unsigned long long* cr3_out);

extern co_rc_t co_kload_begin(co_manager_t* manager, unsigned long long min_va,
			      unsigned long long max_va,
			      unsigned long long ram_bytes);
extern co_rc_t co_kload_chunk(co_manager_t* manager, unsigned long long va,
			      const unsigned char* data, unsigned long size, bool_t zero);
extern co_rc_t co_kload_verify(co_manager_t* manager, unsigned long long va,
			       unsigned long size, unsigned long long* sum_out);
extern co_rc_t co_kload_write(co_manager_t* manager, unsigned long long va,
			      const unsigned char* buf, unsigned long size);
extern co_rc_t co_kload_read(co_manager_t* manager, unsigned long long va,
			     unsigned char* buf, unsigned long size);

/*
 * One aligned 32-bit word, in a single indivisible access.
 *
 * For words the guest is changing underneath us -- ring counters -- where the
 * byte-at-a-time freedom memcpy has is a correctness bug rather than a detail.
 * See the comment on the definitions.
 */
extern co_rc_t co_kload_read_u32(co_manager_t* manager, unsigned long long va,
				 unsigned int* out);
extern co_rc_t co_kload_write_u32(co_manager_t* manager, unsigned long long va,
				  unsigned int value);

/*
 * The same read, holding the teardown lock, for callers outside the driver.
 *
 * Use this from any ioctl handler. co_kload_read is for the in-driver callers
 * that already own the guest for the duration; a user-mode caller does not,
 * and racing a teardown means walking page tables inside pool that has been
 * handed back. The bound exists so a reader cannot hold a teardown for an
 * unbounded time -- ask for more by asking more often.
 */
#define CO_KREAD_MAX_BYTES	(2 * 1024 * 1024)

extern co_rc_t co_kload_read_locked(co_manager_t* manager, unsigned long long va,
				    unsigned char* buf, unsigned long size);
extern co_rc_t co_kload_build_ram(co_manager_t* manager, unsigned long long ram_bytes,
				  unsigned long long text_va, unsigned long long end_va);
extern unsigned long co_kload_ram_pages(void);
extern void    co_kload_free(co_manager_t* manager);

/*
 * The teardown lock. co_kload_free is reachable from the KLOAD_END ioctl, from
 * co_kload_begin, and from the manager's unload path, so it has to be safe to
 * call twice at once -- see the comment on kload_lock in kload.c.
 */
extern co_rc_t co_kload_init(void);
extern void    co_kload_fini(void);

extern co_arch_guest_space_t* co_kload_space(void);
extern unsigned long	      co_kload_pages(void);
extern unsigned long	      co_kload_chunks(void);

#endif
