/*
 * This source code is a part of coLinux source package.
 *
 * Based on definitions from Linux.
 *
 * The code is licensed under the GPL. See the COPYING file at
 * the root directory.
 */

#ifndef __COLINUX_ARCH_X86_64_DEFS_H__
#define __COLINUX_ARCH_X86_64_DEFS_H__

/*
 * CPUID feature bits, leaf 1 EDX.
 */
#define CO_ARCH_X86_FEATURE_FPU       (0)
#define CO_ARCH_X86_FEATURE_PSE       (3)
#define CO_ARCH_X86_FEATURE_PAE       (6)
#define CO_ARCH_X86_FEATURE_APIC      (9)
#define CO_ARCH_X86_FEATURE_SEP       (11)
#define CO_ARCH_X86_FEATURE_PGE       (13)
#define CO_ARCH_X86_FEATURE_FXSR      (24)
#define CO_ARCH_X86_FEATURE_SSE       (25)
#define CO_ARCH_X86_FEATURE_SSE2      (26)

/* Leaf 1 ECX. */
#define CO_ARCH_X86_FEATURE_XSAVE     (26)
#define CO_ARCH_X86_FEATURE_OSXSAVE   (27)

/* Leaf 0x80000001 EDX. */
#define CO_ARCH_X86_FEATURE_NX        (20)
#define CO_ARCH_X86_FEATURE_LM        (29)	/* long mode */

/* Leaf 7 subleaf 0 EBX. */
#define CO_ARCH_X86_FEATURE_FSGSBASE  (0)
#define CO_ARCH_X86_FEATURE_SMEP      (7)
#define CO_ARCH_X86_FEATURE_SMAP      (20)

/*
 * CR0
 */
#define CO_ARCH_X86_CR0_PE		0x00000001	/* protected mode */
#define CO_ARCH_X86_CR0_MP		0x00000002	/* monitor coprocessor */
#define CO_ARCH_X86_CR0_EM		0x00000004	/* emulation */
#define CO_ARCH_X86_CR0_TS		0x00000008	/* task switched */
#define CO_ARCH_X86_CR0_ET		0x00000010	/* extension type */
#define CO_ARCH_X86_CR0_NE		0x00000020	/* numeric error */
#define CO_ARCH_X86_CR0_WP		0x00010000	/* write protect */
#define CO_ARCH_X86_CR0_PG		0x80000000	/* paging */

/*
 * CR4. The i386 header stops at VMXE; the rest matter here because the switch
 * reloads CR4 wholesale and must not silently drop a bit the host had set.
 */
#define CO_ARCH_X86_CR4_VME		0x00000001	/* vm86 extensions */
#define CO_ARCH_X86_CR4_PVI		0x00000002	/* virtual interrupts */
#define CO_ARCH_X86_CR4_TSD		0x00000004	/* disable rdtsc at cpl 3 */
#define CO_ARCH_X86_CR4_DE		0x00000008	/* debugging extensions */
#define CO_ARCH_X86_CR4_PSE		0x00000010	/* page size extensions */
#define CO_ARCH_X86_CR4_PAE		0x00000020	/* mandatory in long mode */
#define CO_ARCH_X86_CR4_MCE		0x00000040	/* machine check */
#define CO_ARCH_X86_CR4_PGE		0x00000080	/* global pages */
#define CO_ARCH_X86_CR4_PCE		0x00000100	/* perf counters at cpl 3 */
#define CO_ARCH_X86_CR4_OSFXSR		0x00000200	/* fxsave/fxrstor */
#define CO_ARCH_X86_CR4_OSXMMEXCPT	0x00000400	/* unmasked SSE exceptions */
#define CO_ARCH_X86_CR4_UMIP		0x00000800	/* restrict sgdt/sidt/etc */
#define CO_ARCH_X86_CR4_LA57		0x00001000	/* 5-level paging */
#define CO_ARCH_X86_CR4_VMXE		0x00002000	/* VMX */
#define CO_ARCH_X86_CR4_SMXE		0x00004000	/* SMX */
#define CO_ARCH_X86_CR4_FSGSBASE	0x00010000	/* rd/wrfsbase */
#define CO_ARCH_X86_CR4_PCIDE		0x00020000	/* process context ids */
#define CO_ARCH_X86_CR4_OSXSAVE		0x00040000	/* xsave */
#define CO_ARCH_X86_CR4_SMEP		0x00100000	/* supervisor exec prevent */
#define CO_ARCH_X86_CR4_SMAP		0x00200000	/* supervisor access prevent */
#define CO_ARCH_X86_CR4_PKE		0x00400000	/* protection keys */
#define CO_ARCH_X86_CR4_CET		0x00800000	/* control flow enforcement */

/*
 * EFER bits. Long mode is a *mode*, held in a register the i386 switch never
 * touches, so EFER has to become part of the saved state.
 */
#define CO_ARCH_X86_EFER_SCE		0x00000001	/* syscall/sysret enable */
#define CO_ARCH_X86_EFER_LME		0x00000100	/* long mode enable */
#define CO_ARCH_X86_EFER_LMA		0x00000400	/* long mode active */
#define CO_ARCH_X86_EFER_NXE		0x00000800	/* no-execute enable */

/*
 * MSR numbers, as strings for rdmsr/wrmsr in the passage assembly.
 *
 * The i386 switch saves only the three SYSENTER MSRs, and truncates them to 32
 * bits by zeroing EDX before wrmsr. In long mode each of these is a full 64-bit
 * value that must be preserved as an EDX:EAX pair, and the SYSCALL and
 * segment-base MSRs below have to be saved as well. FS_BASE, GS_BASE and
 * KERNEL_GS_BASE are the critical ones: writing a selector to %fs or %gs does
 * not restore the 64-bit base, and Windows keeps the KPCR at GS base -- so
 * restoring only selectors, as the i386 code does, would destroy the host's
 * per-CPU pointer. See doc/porting-x86_64 section 4.2.
 */
#define MSR_IA32_SYSENTER_CS		"0x00000174"
#define MSR_IA32_SYSENTER_ESP		"0x00000175"
#define MSR_IA32_SYSENTER_EIP		"0x00000176"

#define MSR_IA32_EFER			"0xc0000080"
#define MSR_IA32_STAR			"0xc0000081"
#define MSR_IA32_LSTAR			"0xc0000082"
#define MSR_IA32_CSTAR			"0xc0000083"
#define MSR_IA32_SFMASK			"0xc0000084"
#define MSR_IA32_FS_BASE		"0xc0000100"
#define MSR_IA32_GS_BASE		"0xc0000101"
#define MSR_IA32_KERNEL_GS_BASE		"0xc0000102"

/* Same values, usable from C. */
#define CO_MSR_IA32_EFER		0xc0000080UL
#define CO_MSR_IA32_STAR		0xc0000081UL
#define CO_MSR_IA32_LSTAR		0xc0000082UL
#define CO_MSR_IA32_CSTAR		0xc0000083UL
#define CO_MSR_IA32_SFMASK		0xc0000084UL
#define CO_MSR_IA32_FS_BASE		0xc0000100UL
#define CO_MSR_IA32_GS_BASE		0xc0000101UL
#define CO_MSR_IA32_KERNEL_GS_BASE	0xc0000102UL

#endif
