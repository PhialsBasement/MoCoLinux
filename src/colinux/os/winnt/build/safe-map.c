/*
 * Exception-safe user mapping, linked directly into linux.sys.
 *
 * MmMapLockedPagesSpecifyCache raises when a UserMode mapping cannot be made;
 * BugCheckOnFailure is ignored in that mode. The 2 GB failure proved that a
 * reserve/free VA probe is not a substitute: the probe succeeded, this call
 * raised anyway, and the unhandled exception became KMODE_EXCEPTION_NOT_HANDLED.
 *
 * GCC has no C syntax for Windows __try/__except. MinGW's __try1 macro emits
 * inline scope labels that optimisation can move the protected call outside
 * of, so x64 uses a fixed assembly prologue and scope table. This is the shape
 * clang-cl emits for:
 *
 *     __try { return MmMapLockedPagesSpecifyCache(... UserMode ...); }
 *     __except (EXCEPTION_EXECUTE_HANDLER) { return NULL; }
 *
 * This file deliberately lives in the final build directory rather than the
 * lowlevel aggregate. The project's intermediate `ld -r` links damage PE x64
 * .pdata relocations; direct final linking preserves the runtime-function and
 * unwind records that make the handler operative.
 */

#include "../kernel/ddk.h"

#if defined(__x86_64__)

extern PVOID co_os_map_locked_pages_user_safe(PMDL mdl);

asm(
".text\n"
".p2align 4\n"
".globl co_os_map_locked_pages_user_safe\n"
".def co_os_map_locked_pages_user_safe; .scl 2; .type 32; .endef\n"
".seh_proc co_os_map_locked_pages_user_safe\n"
"co_os_map_locked_pages_user_safe:\n"
"pushq %rbp\n"
".seh_pushreg %rbp\n"
"subq $48, %rsp\n"
".seh_stackalloc 48\n"
"leaq 48(%rsp), %rbp\n"
".seh_setframe %rbp, 48\n"
".seh_endprologue\n"
".Lco_map_try_begin:\n"
"movl $32, 40(%rsp)\n"             /* HighPagePriority, argument 6 */
"movb $0, 32(%rsp)\n"              /* BugCheckOnFailure, argument 5 */
"movb $1, %dl\n"                   /* UserMode */
"movl $1, %r8d\n"                  /* MmCached */
"xorl %r9d, %r9d\n"                /* RequestedAddress == NULL */
"call *__imp_MmMapLockedPagesSpecifyCache(%rip)\n"
"nop\n"                             /* keep the unwound return PC in scope */
".Lco_map_try_end:\n"
"addq $48, %rsp\n"
"popq %rbp\n"
"ret\n"
".Lco_map_exception:\n"
"xorl %eax, %eax\n"
"addq $48, %rsp\n"
"popq %rbp\n"
"ret\n"
".seh_handler __C_specific_handler, @unwind, @except\n"
".seh_handlerdata\n"
".long 1\n"                         /* one protected scope */
".rva .Lco_map_try_begin, .Lco_map_try_end\n"
".long 1\n"                         /* EXCEPTION_EXECUTE_HANDLER */
".rva .Lco_map_exception\n"
".text\n"
".seh_endproc\n"
);

#else

PVOID co_os_map_locked_pages_user_safe(PMDL mdl)
{
	return MmMapLockedPagesSpecifyCache(mdl, UserMode, MmCached, NULL,
					    FALSE, HighPagePriority);
}

#endif
