/*
 * This source code is a part of coLinux source package.
 *
 * Based on definitions from Linux.
 *
 * The code is licensed under the GPL. See the COPYING file at
 * the root directory.
 */

#ifndef __COLINUX_ARCH_X86_64_MMU_H__
#define __COLINUX_ARCH_X86_64_MMU_H__

/*
 * Long mode paging geometry. Note what is *not* here: there is no non-PAE case
 * and no PAE case. Long mode has exactly one paging format, with 8-byte entries
 * and 512 per table at every level, so the CO_ARCH_PAE_* definitions the i386
 * port needs have no counterpart. The 32-bit passage code carries two variants
 * of the world switch precisely to toggle CR4.PAE between a PAE host and a
 * non-PAE guest; with both sides in long mode there is nothing to toggle.
 * See doc/porting-x86_64 section 4.1.
 */

/* Physical frame numbers span the host's full physical address space. */
typedef unsigned long long co_pfn_t;

#define _KERNPG_TABLE   (_PAGE_PRESENT | _PAGE_RW | _PAGE_ACCESSED | _PAGE_DIRTY)

#ifndef _PAGE_PRESENT
#define _PAGE_PRESENT   0x001
#define _PAGE_RW        0x002
#define _PAGE_USER      0x004
#define _PAGE_PWT       0x008
#define _PAGE_PCD       0x010
#define _PAGE_ACCESSED  0x020
#define _PAGE_DIRTY     0x040
#define _PAGE_PSE       0x080   /* 2 MB page at PD level, 1 GB at PDPT level */
#define _PAGE_GLOBAL    0x100
#endif /* !_PAGE_PRESENT */
#define CO_ARCH_PAGE_NX        0x8000000000000000ULL

#define CO_ARCH_PAGE_SHIFT      12
#define CO_ARCH_PAGE_SIZE       (1UL << CO_ARCH_PAGE_SHIFT)

/*
 * 64-bit explicitly. The obvious ~(CO_ARCH_PAGE_SIZE-1) is a trap here: Win64 is
 * LLP64, so CO_ARCH_PAGE_SIZE's `1UL` is 32 bits, the complement is 0xFFFFF000,
 * and masking a 64-bit address with it zero-extends to 0x00000000FFFFF000 --
 * discarding everything above 4 GB. Generic code (monitor.c, transfer.c,
 * pages.c) masks 64-bit guest addresses with this, so the width has to be stated
 * here rather than inherited from the host's word size. The i386 mmu.h keeps the
 * narrow form, which is correct there.
 */
#define CO_ARCH_PAGE_MASK       (~((unsigned long long)(CO_ARCH_PAGE_SIZE - 1)))

/*
 * Four levels, 512 eight-byte entries each. Linux names them, from the top:
 * PGD (the PML4), PUD (the page directory pointer table), PMD (the page
 * directory) and PTE.
 */
#define CO_ARCH_PTE_SHIFT       12
#define CO_ARCH_PMD_SHIFT       21
#define CO_ARCH_PUD_SHIFT       30
#define CO_ARCH_PGD_SHIFT       39

#define CO_ARCH_PTRS_PER_PTE    512
#define CO_ARCH_PTRS_PER_PMD    512
#define CO_ARCH_PTRS_PER_PUD    512
#define CO_ARCH_PTRS_PER_PGD    512

#define CO_ARCH_PMD_SIZE        (1ULL << CO_ARCH_PMD_SHIFT)
#define CO_ARCH_PMD_MASK        (~(CO_ARCH_PMD_SIZE-1))
#define CO_ARCH_PUD_SIZE        (1ULL << CO_ARCH_PUD_SHIFT)
#define CO_ARCH_PUD_MASK        (~(CO_ARCH_PUD_SIZE-1))
#define CO_ARCH_PGD_SIZE        (1ULL << CO_ARCH_PGD_SHIFT)
#define CO_ARCH_PGD_MASK        (~(CO_ARCH_PGD_SIZE-1))

/* Index of a virtual address at each level. */
#define CO_ARCH_PTE_INDEX(va)   (((va) >> CO_ARCH_PTE_SHIFT) & (CO_ARCH_PTRS_PER_PTE-1))
#define CO_ARCH_PMD_INDEX(va)   (((va) >> CO_ARCH_PMD_SHIFT) & (CO_ARCH_PTRS_PER_PMD-1))
#define CO_ARCH_PUD_INDEX(va)   (((va) >> CO_ARCH_PUD_SHIFT) & (CO_ARCH_PTRS_PER_PUD-1))
#define CO_ARCH_PGD_INDEX(va)   (((va) >> CO_ARCH_PGD_SHIFT) & (CO_ARCH_PTRS_PER_PGD-1))

/*
 * Names the i386 header defines unconditionally. Kept so shared code compiles,
 * but pointed at the long-mode levels: PGDIR_SHIFT is the top level, and a
 * "PMD" is the 2 MB-covering page directory rather than i386's 4 MB one.
 */
#define PTRS_PER_PTE            CO_ARCH_PTRS_PER_PTE
#define PTRS_PER_PGD            CO_ARCH_PTRS_PER_PGD
#define PGDIR_SHIFT             CO_ARCH_PGD_SHIFT
#define PMD_MASK                CO_ARCH_PMD_MASK

/*
 * Linux x86-64 splits what i386 calls PAGE_OFFSET in two: the direct map of
 * physical memory, and a separate 2 GB window the kernel image itself lives in.
 * 0xffff880000000000 is the 2.6.x value; it moved to 0xffff888000000000 when
 * KASLR arrived, so this has to be revisited for a modern guest (M6).
 */
/*
 * PAGE_OFFSET, the base of the linear map of all physical memory.
 *
 * 0xffff880000000000 is the 2.6.x value and is what CO_ARCH_KERNEL_OFFSET has
 * always meant here. Modern kernels moved it to 0xffff888000000000 to make room
 * for the PTI LDT remap below it, so the two are kept apart rather than one
 * being quietly redefined -- they differ by 512 GB and picking the wrong one
 * produces a direct map that is entirely absent rather than slightly wrong.
 */
#define CO_ARCH_KERNEL_OFFSET   0xffff880000000000ULL	/* 2.6.x  PAGE_OFFSET */
#define CO_ARCH_DIRECT_MAP      0xffff888000000000ULL	/* modern PAGE_OFFSET */
#define CO_ARCH_KERNEL_MAP      0xffffffff80000000ULL	/* __START_KERNEL_map */

/*
 * Canonical-address test. Bits 63:48 must all equal bit 47, so there is a
 * non-addressable hole in the middle of the space. Anything that computes an
 * address by adding an offset -- which the passage code does when it relocates
 * itself between mappings -- can land in it, and touching it raises #GP rather
 * than a page fault.
 */
#define CO_ARCH_VA_CANONICAL(va) \
	((((va) >> 47) == 0) || (((va) >> 47) == 0x1ffffULL))

#endif
