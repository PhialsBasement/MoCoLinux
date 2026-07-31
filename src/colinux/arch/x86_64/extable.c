/*
 * This source code is a part of coLinux source package.
 *
 * The code is licensed under the GPL. See the COPYING file at
 * the root directory.
 */

/*
 * Applying the guest kernel's exception table from the host.
 *
 * Linux faults on purpose, constantly, and expects to recover. __switch_to
 * loads a segment register with a value that may no longer be valid; rdmsr_safe
 * reads a register the processor may not implement; every copy to a user
 * address may find it unmapped. Each of those is an instruction with an entry
 * in __ex_table beside it, and the kernel's #GP and #PF handlers consult that
 * table, adjust a register, move RIP to where the entry says, and carry on.
 *
 * A cooperative guest runs on the host's IDT deliberately, so those handlers do
 * not exist for it, and the first such fault ends the boot. It ended this one
 * at 147,341,911 instructions, on
 *
 *     mov %edx,%es        __switch_to+0x3e3
 *
 * which is __loadsegment_es(), written as
 *
 *     1: movl %k0,%%es
 *        _ASM_EXTABLE_TYPE_REG(1b, 1b, EX_TYPE_ZERO_REG, %k0)
 *
 * -- if that selector is refused, zero the register and run the instruction
 * again, which then loads the null selector and always succeeds. Nothing is
 * wrong when it faults. The fault is the mechanism.
 *
 * So the host reads the table and does what the handler would have done. It is
 * the same arrangement as the bug table behind WARN_ON: the kernel writes down
 * what it wants, and the part that reads it is not running here.
 *
 * The table is copied into host memory once per run rather than walked in the
 * guest per fault. It is about eighteen kilobytes and it never changes, and a
 * copy can be checked for sortedness once instead of trusted every time.
 */

/* GCC's own freestanding stddef.h, for size_t as a pointer-sized integer. */
#include <stddef.h>

#include <colinux/common/common.h>
#include <colinux/common/debug.h>
#include <colinux/common/libc.h>
#include <colinux/os/kernel/alloc.h>
#include <colinux/kernel/kload.h>

#include "extable.h"

/*
 * One entry, exactly as the kernel lays it out:
 *
 *	struct exception_table_entry { int insn, fixup, data; };
 *
 * insn and fixup are displacements from the address of their own field, which
 * is what makes the table position independent. data packs a type, a register
 * number and a signed immediate.
 */
typedef struct {
	int insn;
	int fixup;
	int data;
} co_ex_entry_t;

#define CO_EX_ENTRY_SIZE	12

/* data field, from arch/x86/include/asm/extable_fixup_types.h */
#define CO_EX_TYPE(d)		((d) & 0xff)
#define CO_EX_REG(d)		(((d) >> 8) & 0xf)
#define CO_EX_FLAGS(d)		(((d) >> 12) & 0xf)
#define CO_EX_IMM(d)		((int)(d) >> 16)	/* signed */

#define CO_EX_FLAG_CLEAR_AX	1
#define CO_EX_FLAG_CLEAR_DX	2

#define CO_EX_TYPE_NONE			0
#define CO_EX_TYPE_DEFAULT		1
#define CO_EX_TYPE_FAULT		2
#define CO_EX_TYPE_UACCESS		3
#define CO_EX_TYPE_CLEAR_FS		5
#define CO_EX_TYPE_FPU_RESTORE		6
#define CO_EX_TYPE_BPF			7
#define CO_EX_TYPE_WRMSR		8
#define CO_EX_TYPE_RDMSR		9
#define CO_EX_TYPE_WRMSR_SAFE		10
#define CO_EX_TYPE_RDMSR_SAFE		11
#define CO_EX_TYPE_WRMSR_IN_MCE		12
#define CO_EX_TYPE_RDMSR_IN_MCE		13
#define CO_EX_TYPE_DEFAULT_MCE_SAFE	14
#define CO_EX_TYPE_FAULT_MCE_SAFE	15
#define CO_EX_TYPE_POP_REG		16
#define CO_EX_TYPE_IMM_REG		17
#define CO_EX_TYPE_FAULT_SGX		18
#define CO_EX_TYPE_UCOPY_LEN		19
#define CO_EX_TYPE_ZEROPAD		20
#define CO_EX_TYPE_ERETU		21

/*
 * Where each register sits in the stub's saved frame.
 *
 * The table's register numbers are the assembler's, from the .irp in
 * asm/asm.h: rax, rcx, rdx, rbx, rsp, rbp, rsi, rdi, r8..r15. The stub pushes
 * in its own order and the CPU pushed RSP before any of it, so the two have to
 * be mapped rather than assumed equal -- writing the wrong one here would
 * corrupt a register quietly and be found a long way from the cause.
 *
 * Frame layout, from the comment on co_switch_guest_fault:
 *   0x00 r15  0x08 r14  0x10 r13  0x18 r12  0x20 r11  0x28 r10
 *   0x30 r9   0x38 r8   0x40 rbp  0x48 rdi  0x50 rsi  0x58 rdx
 *   0x60 rcx  0x68 rbx  0x70 rax
 *   0x78 vector  0x80 error code
 *   0x88 RIP  0x90 CS  0x98 RFLAGS  0xa0 RSP  0xa8 SS
 */
static const int co_ex_reg_offset[16] = {
	0x70,	/*  0 rax */
	0x60,	/*  1 rcx */
	0x58,	/*  2 rdx */
	0x68,	/*  3 rbx */
	0xa0,	/*  4 rsp -- in the CPU's frame, not among the pushes */
	0x40,	/*  5 rbp */
	0x50,	/*  6 rsi */
	0x48,	/*  7 rdi */
	0x38,	/*  8 r8  */
	0x30,	/*  9 r9  */
	0x28,	/* 10 r10 */
	0x20,	/* 11 r11 */
	0x18,	/* 12 r12 */
	0x10,	/* 13 r13 */
	0x08,	/* 14 r14 */
	0x00,	/* 15 r15 */
};

#define CO_EX_FRAME_RIP		0x88

static co_ex_entry_t*	  co_ex_table;
static int		  co_ex_count;
static unsigned long long co_ex_start_va;
static bool_t		  co_ex_sorted;

int co_arch_extable_count(void)
{
	return co_ex_count;
}

/* The guest virtual address entry i's insn field refers to. */
static unsigned long long co_ex_insn_addr(int i)
{
	return co_ex_start_va + (unsigned long long)i * CO_EX_ENTRY_SIZE
	       + (long long)co_ex_table[i].insn;
}

/* And where it says to resume. The fixup field is the second of the three. */
static unsigned long long co_ex_fixup_addr(int i)
{
	return co_ex_start_va + (unsigned long long)i * CO_EX_ENTRY_SIZE + 4
	       + (long long)co_ex_table[i].fixup;
}

co_rc_t co_arch_extable_load(co_manager_t* manager, unsigned long long start_va,
			     unsigned long long stop_va)
{
	unsigned long bytes;
	co_rc_t rc;
	int i;

	co_arch_extable_free();

	if (start_va == 0 || stop_va <= start_va)
		return CO_RC(INVALID_PARAMETER);

	bytes = (unsigned long)(stop_va - start_va);
	if (bytes % CO_EX_ENTRY_SIZE) {
		co_debug_error("extable: %ld bytes is not a whole number of "
			       "%d-byte entries", bytes, CO_EX_ENTRY_SIZE);
		return CO_RC(INVALID_PARAMETER);
	}

	co_ex_table = co_os_malloc(bytes);
	if (co_ex_table == NULL)
		return CO_RC(OUT_OF_MEMORY);

	rc = co_kload_read(manager, start_va, (unsigned char*)co_ex_table, bytes);
	if (!CO_OK(rc)) {
		co_os_free(co_ex_table);
		co_ex_table = NULL;
		co_debug_error("extable: could not read 0x%llx..0x%llx",
			       start_va, stop_va);
		return rc;
	}

	co_ex_start_va = start_va;
	co_ex_count    = (int)(bytes / CO_EX_ENTRY_SIZE);

	/*
	 * Sorted by scripts/sorttable at build time, which is what makes a
	 * binary search legitimate. Checked rather than assumed, once, because
	 * a binary search over an unsorted table does not fail -- it silently
	 * fails to find entries that are there, and every recoverable fault
	 * would then become a dead boot with no explanation.
	 */
	co_ex_sorted = PTRUE;
	for (i = 1; i < co_ex_count; i++) {
		if (co_ex_insn_addr(i) < co_ex_insn_addr(i - 1)) {
			co_ex_sorted = PFALSE;
			break;
		}
	}

	co_debug("extable: %d entries from 0x%llx, %s",
		 co_ex_count, start_va,
		 co_ex_sorted ? "sorted" : "NOT sorted -- searching linearly");

	return CO_RC(OK);
}

void co_arch_extable_free(void)
{
	if (co_ex_table)
		co_os_free(co_ex_table);

	co_ex_table    = NULL;
	co_ex_count    = 0;
	co_ex_start_va = 0;
	co_ex_sorted   = PFALSE;
}

static int co_ex_find(unsigned long long rip)
{
	int lo, hi;

	if (co_ex_table == NULL)
		return -1;

	if (!co_ex_sorted) {
		int i;

		for (i = 0; i < co_ex_count; i++)
			if (co_ex_insn_addr(i) == rip)
				return i;
		return -1;
	}

	lo = 0;
	hi = co_ex_count - 1;
	while (lo <= hi) {
		int mid = lo + (hi - lo) / 2;
		unsigned long long at = co_ex_insn_addr(mid);

		if (at == rip)
			return mid;
		if (at < rip)
			lo = mid + 1;
		else
			hi = mid - 1;
	}

	return -1;
}

bool_t co_arch_extable_fixup(unsigned long long* frame, unsigned long long vector,
			     int* type_out)
{
	int i, type, reg, flags, imm;

	if (type_out)
		*type_out = CO_EX_TYPE_NONE;

	if (frame == NULL)
		return PFALSE;

	i = co_ex_find(frame[CO_EX_FRAME_RIP / 8]);
	if (i < 0)
		return PFALSE;

	type  = CO_EX_TYPE(co_ex_table[i].data);
	reg   = CO_EX_REG(co_ex_table[i].data);
	flags = CO_EX_FLAGS(co_ex_table[i].data);
	imm   = CO_EX_IMM(co_ex_table[i].data);

	if (type_out)
		*type_out = type;

	/*
	 * Everything below mirrors fixup_exception() in arch/x86/mm/extable.c.
	 * Types that are not implemented are refused rather than approximated:
	 * a fixup that does nearly the right thing leaves the guest running on
	 * a register that is nearly right, which is worse than stopping.
	 */
	switch (type) {
	case CO_EX_TYPE_DEFAULT:
	case CO_EX_TYPE_DEFAULT_MCE_SAFE:
	case CO_EX_TYPE_UACCESS:
		break;

	case CO_EX_TYPE_FAULT:
	case CO_EX_TYPE_FAULT_MCE_SAFE:
		/* ex_handler_fault: the trap number goes back in rax */
		frame[co_ex_reg_offset[0] / 8] = vector;
		break;

	case CO_EX_TYPE_WRMSR_SAFE:
	case CO_EX_TYPE_RDMSR_SAFE:
		/*
		 * A read that failed reports zero, as the kernel's handler
		 * does, and both report -EIO in the named register.
		 */
		if (type == CO_EX_TYPE_RDMSR_SAFE) {
			frame[co_ex_reg_offset[0] / 8] = 0;	/* rax */
			frame[co_ex_reg_offset[2] / 8] = 0;	/* rdx */
		}
		frame[co_ex_reg_offset[reg] / 8] = (unsigned long long)(long long)-5;
		break;

	case CO_EX_TYPE_POP_REG:
		frame[0xa0 / 8] += 8;			/* regs->sp += sizeof(long) */
		frame[co_ex_reg_offset[reg] / 8] = (unsigned long long)(long long)imm;
		break;

	case CO_EX_TYPE_IMM_REG:
		frame[co_ex_reg_offset[reg] / 8] = (unsigned long long)(long long)imm;
		break;

	case CO_EX_TYPE_UCOPY_LEN:
		/* cx := reg + imm*cx */
		frame[co_ex_reg_offset[1] / 8] =
			(unsigned long long)((long long)imm
					     * (long long)frame[co_ex_reg_offset[1] / 8])
			+ frame[co_ex_reg_offset[reg] / 8];
		break;

	default:
		/*
		 * CLEAR_FS wants a segment register written, which is not in
		 * the stub's frame and would be the host's %fs if written here.
		 * ZEROPAD and UCOPY need the faulting instruction decoded.
		 * FPU_RESTORE, BPF, SGX, ERETU and the MCE variants each want
		 * kernel state this side does not have. None has been needed
		 * yet; the point of naming the type is that the first one to
		 * be needed says so.
		 */
		return PFALSE;
	}

	if (flags & CO_EX_FLAG_CLEAR_AX)
		frame[co_ex_reg_offset[0] / 8] = 0;
	if (flags & CO_EX_FLAG_CLEAR_DX)
		frame[co_ex_reg_offset[2] / 8] = 0;

	/*
	 * Last, because some entries resume at the faulting instruction rather
	 * than after it -- a segment load whose register has just been zeroed
	 * is meant to run again and succeed on the null selector.
	 */
	frame[CO_EX_FRAME_RIP / 8] = co_ex_fixup_addr(i);

	return PTRUE;
}
