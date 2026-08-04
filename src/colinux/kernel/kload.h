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
/*
 * Many small blocks rather than a few large ones.
 *
 * Nothing about this design needs a large contiguous run except block 0, and
 * asking for one anyway is what kept taking the host down. The chunk size was
 * 32 MB with a cap of 16 blocks, so 512 MB was already at the ceiling and 1 GB
 * could not be reached at all; worse, a 32 MB contiguous request is among the
 * first things a machine that has been up for a while cannot satisfy, and on a
 * 4 GB box with a browser running, asking for thirty-two of them in a row
 * exhausts it. Falling short is reported rather than fatal, so the symptom was
 * a guest with half its configured memory, or a host that died trying.
 *
 * 8 MB is small enough to be readily available out of a fragmented free pool
 * and large enough that 1 GB still fits inside the e820 the guest can hold.
 * That last part is the real bound and it belongs to the guest, not to us:
 * E820_MAX_ENTRIES is 128 + 3 * MAX_NUMNODES, and with NUMA off in this config
 * MAX_NUMNODES is 1, so the guest can hold 131 ranges and nothing we do on this
 * side changes that. 126 chunks of 8 MB plus block 0 is a little over a
 * gigabyte, which is why the two numbers below are what they are.
 *
 * Going smaller than 8 MB means more ranges than boot_params can carry, which
 * needs the SETUP_E820_EXT setup_data node written into guest memory *and* a
 * guest patch raising E820_MAX_ENTRIES. Worth doing; not done here.
 *
 * What none of this removes is block 0, which holds the image and the page
 * tables and must be one unbroken run of about 44 MB, because __pa() is linear
 * across the image while guest physical equals host physical. Only
 * pseudo-physical memory -- a p2m/machine-frame layer, which coLinux's i386
 * port and Xen both have and this port deferred -- removes that, and with it
 * the last contiguous allocation in the design.
 */
#define CO_KLOAD_RAM_BYTES	(128ULL << 20)
/*
 * How large a piece to ask the host for at a time.
 *
 * Every one of these is an MmAllocateContiguousMemory call, and that is not a
 * cheap allocation: it searches the PFN database for an unbroken physical run,
 * with the memory manager's locks held, and on a machine that is already full
 * it will trim working sets to find one. The cost is per call, not per byte.
 *
 * This was dropped to 8 MB when the block cap went up, on the theory that
 * smaller pieces are easier to find. They are, but a gigabyte then takes a
 * hundred and twenty seven of these searches instead of thirty two, and the
 * host is unusable for the whole sequence -- the machine went sluggish the
 * instant the daemon was started, before the guest had executed an
 * instruction. Four times the calls for the same total is a bad trade for
 * memory that only has to be contiguous per block.
 *
 * 32 MB again, and the fallback below already halves on refusal, down to 2 MB,
 * so a fragmented host still gets served -- it simply starts by asking for
 * something worth the search. The high block cap stays, because that is what
 * lets those halvings accumulate to a full gigabyte; it was never the cap that
 * was expensive.
 */
#define CO_KLOAD_CHUNK_BYTES	(32ULL << 20)
#define CO_KLOAD_MAX_BLOCKS	127

/*
 * The blocks themselves, for the KMAP path, plus the reference count that
 * keeps teardown from freeing pages a user mapping still names. See the
 * comment on kload_user_maps in kload.c.
 */
extern int	 co_kload_block_count(void);
extern co_rc_t	 co_kload_block(int i, void** va, unsigned long long* pa,
				unsigned long long* bytes);
extern void	 co_kload_user_map_get(void);
extern void	 co_kload_user_map_put(co_manager_t* manager);

extern unsigned long long co_kload_phys_base(void);
extern int		  co_kload_range_count(void);
extern void		  co_kload_range(int i, unsigned long long* pa,
					 unsigned long long* usable,
					 unsigned long long* reserved);
extern co_rc_t		  co_kload_table_frame(co_manager_t* manager, co_pfn_t* pfn_out);
extern void*		  co_kload_frame_va(co_pfn_t pfn);
/*
 * One guest virtual address resolved once to a host pointer, for readers
 * that cannot afford a walk per access. Valid only while the guest is up --
 * see the implementation for the lifetime rule.
 */
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
extern co_rc_t co_kload_write(co_manager_t* manager, unsigned long long va,
			      const unsigned char* buf, unsigned long size);
extern co_rc_t co_kload_read(co_manager_t* manager, unsigned long long va,
			     unsigned char* buf, unsigned long size);

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
