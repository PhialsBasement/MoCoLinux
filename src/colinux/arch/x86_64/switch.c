/*
 * This source code is a part of coLinux source package.
 *
 * The code is licensed under the GPL. See the COPYING file at
 * the root directory.
 */

/*
 * The first half of a world switch: change CR3 and keep executing.
 *
 * Everything the passage code does rests on one property -- that the page the
 * code lives in is mapped at the same virtual address in both address spaces, so
 * instruction fetch continues across the CR3 write. This tests exactly that and
 * nothing else. No state is saved or restored, no control is transferred, and
 * the guest is not a kernel; it is a store instruction.
 *
 * The window between the two CR3 writes is unforgiving. The guest address space
 * maps precisely one page, so anything else touched in that window -- the stack,
 * the driver's own code, an interrupt vectoring through the host's IDT to a
 * handler that is no longer mapped -- is a page fault whose handler is also
 * unmapped, which is a double fault, which is a triple fault, which is a reset.
 * So the window contains no stack access, no call, no memory reference outside
 * the passage page, and runs with interrupts off.
 *
 * CR4.PGE is cleared first, and that is not incidental. PGE is set on this host
 * and Windows marks its kernel pages global, so global TLB entries survive a CR3
 * write. Without flushing them the host's mappings would remain live inside the
 * guest address space and the test would pass whether or not the guest page
 * tables were correct -- which is worse than no test. This is the same reason the
 * i386 passage code clears PGE on its way through.
 */

/* GCC's own freestanding stddef.h, for size_t as a pointer-sized integer. */
#include <stddef.h>

#include <colinux/common/common.h>
#include <colinux/common/debug.h>
#include <colinux/common/libc.h>
#include <colinux/kernel/manager.h>
#include <colinux/os/kernel/alloc.h>
#include <colinux/os/kernel/misc.h>
#include <colinux/os/kernel/time.h>
#include <colinux/kernel/net.h>
#include <colinux/os/timer.h>
#include <colinux/arch/switch.h>
#include <colinux/arch/state.h>
#include <colinux/arch/space.h>
#include <colinux/kernel/kload.h>
#include <colinux/kernel/cobd.h>
#include <colinux/kernel/cobd_async.h>
#include <colinux/kernel/console.h>

#include "mmu.h"
#include "utils.h"
#include "defs.h"
#include "extable.h"

/* x86-64 extension to the original co_operation_t wire values. */
#define CO_OPERATION_YIELD		16
#define CO_OPERATION_BLOCK_PROBE	17
#define CO_OPERATION_BLOCK_IO		18
/*
 * Start a secondary processor. The guest fills params[74..78] with the state
 * the new processor is to begin in and crosses; the host hands that to the
 * thread already waiting for it. The cooperative stand-in for INIT-SIPI.
 */
#define CO_OPERATION_START_VCPU		19
/*
 * Wake a processor that has a message waiting. The guest has already posted
 * the vector into that processor's pending set; this only rings the doorbell,
 * because a target sitting in the host's idle wait has nothing to re-enter it
 * and a message nobody is told about is a hang. params[74] names the target.
 */
#define CO_OPERATION_KICK_VCPU		20
#define CO_PP_AP_VCPU			74
#define CO_PP_AP_RIP			75
#define CO_PP_AP_RSP			76
#define CO_PP_AP_CR3			77
#define CO_PP_AP_GS_BASE		78

/*
 * The guest's tick rate, and why the HOST has to know it.
 *
 * This is not a tuning constant. The guest has no timer hardware: its
 * clockevent device is CLOCK_EVT_FEAT_PERIODIC at HZ (arch/x86/kernel/
 * process.c), and its events are synthesised by co_colinux_take_ticks(), which
 * reads the host monotonic clock out of the passage page and turns elapsed time
 * into that many calls of the tick handler. That function only runs when this
 * vCPU's host thread wakes up and re-enters the guest.
 *
 * So the host's idle wait is not merely a doorbell backstop -- it IS the
 * guest's clock. A vCPU whose host thread blocks receives no ticks at all, and
 * every msleep(), schedule_timeout() and timer_list on that processor stops
 * expiring until some unrelated doorbell happens to wake it. Measured exactly
 * that way: an indefinite wait moved "EXT4-fs (cobd0) mounted" from 0.348s to
 * 13.173s and left systemd sitting in 90-second start-job timeouts.
 *
 * A backoff ladder is the same bug in slower motion. The previous
 * "idle_run > 100 ? 10 : 1" quietly ran the guest's clock at 100 Hz whenever a
 * processor had been idle a while, and the secondary's flat 100 ms ran it at
 * 10 Hz for the whole of that processor's life.
 *
 * The honest bound is the tick period itself, derived from HZ in one place and
 * used both by the injection gate and by the idle wait. When the guest learns
 * to publish its next expiry (a oneshot/NO_HZ clockevent), this becomes a
 * fallback rather than the rate.
 */
#define CO_GUEST_HZ		1000
#define CO_GUEST_TICK_MS	(1000u / CO_GUEST_HZ)
#define CO_GUEST_TICK_100NS	(10000000ULL / CO_GUEST_HZ)

/*
 * What a crossing loop needs to RUN, as against what a boot needs to SET UP.
 *
 * The division is not stylistic, and it is what makes one loop serve every
 * processor. co_arch_boot_loaded()'s inputs are of two kinds: addresses used
 * exactly once, before any processor executes -- the entry point, the kernel's
 * page tables, the console ring symbol, the exception table, the passage
 * symbol -- and policy the loop consults on every iteration. Only the second
 * kind can possibly be needed by a processor other than the boot one, so only
 * the second kind is here.
 *
 * Built once, by the boot processor, before any secondary can be started, and
 * read-only afterwards. That is what lets both loops share a pointer to it with
 * no lock.
 *
 * This replaces cobd_async_enabled and cobd_io_va_shared, the two file-scope
 * statics that were added one at a time as each secondary failure named them --
 * cobd0 never appearing, and the boot standing still one line after "loop:
 * module loaded" waiting for a disk that never registered. Two ad-hoc copies of
 * two fields could drift from the boot block; one struct both loops read
 * cannot, and the next field a secondary turns out to need is added here rather
 * than discovered on hardware.
 */
typedef struct {
	unsigned long	   max_switches;	/* 0: no limit */
	int		   step;
	int		   batch;
	unsigned long	   deadline_secs;	/* 0: no deadline */
	/* asm_sysvec_co_timer, and the BASE of co_colinux_virtual_if[] */
	unsigned long long tick_entry_va;
	unsigned long long virtual_if_va;
	int		   async_cobd;
	unsigned long long cobd_io_va;
	/*
	 * Stop a headless bring-up run after this many cooperative idle yields.
	 * Zero on a secondary: a run reports through the boot processor, and a
	 * secondary that stopped counting idles of its own would leave the guest
	 * a processor short with nothing said about it.
	 */
	unsigned long	   idle_stop_after;
} co_vcpu_ctl_t;

/*
 * What a boot allocates and a teardown has to give back.
 *
 * Carried between co_arch_boot_prepare() and co_arch_boot_loaded() so that the
 * cleanup lives in ONE place instead of behind two fall-through labels. The
 * old arrangement -- out_free_stack falling into out_free_pp -- worked, but it
 * is the shape that leaks the moment somebody adds an early exit above the
 * label they were thinking of, and the prepare half now has six of them. Every
 * field is zero until the thing it names exists, so the cleanup is
 * unconditional and idempotent.
 */
typedef struct {
	co_arch_passage_page_t*	pp;
	struct co_console_ring*	ring;
	co_pfn_t		stack_pfn;
} co_boot_ctx_t;

/*
 * Position independent by construction: no RIP-relative operand, no absolute
 * address, no symbol reference. It is copied into the passage page and called
 * there, so it must not care where it lives.
 *
 * Win64 argument registers: rcx, rdx, r8, r9.
 *   rcx = host CR3      rdx = guest CR3
 *   r8  = sentinel address, inside the passage page
 *   r9  = value to store
 */
extern char co_switch_probe_code;
extern char co_switch_probe_code_end;

asm(".text                                          \n"
    ".globl co_switch_probe_code                    \n"
    "co_switch_probe_code:                          \n"
    "    pushfq                                     \n"
    "    cli                                        \n"
    /* flush global TLB entries, or the host stays mapped and the test lies */
    "    mov %cr4, %rax                             \n"
    "    mov %rax, %r10                             \n"
    "    btr $7, %rax                               \n"   /* CR4.PGE */
    "    mov %rax, %cr4                             \n"
    /* --- into the guest address space --- */
    "    mov %rdx, %cr3                             \n"
    /* the only memory reference permitted here, and it is in the mapped page */
    "    mov %r9, (%r8)                             \n"
    /* --- back to the host --- */
    "    mov %rcx, %cr3                             \n"
    "    mov %r10, %rax                             \n"
    "    mov %rax, %cr4                             \n"
    "    popfq                                      \n"
    "    ret                                        \n"
    ".globl co_switch_probe_code_end                \n"
    "co_switch_probe_code_end:                      \n");


/*
 * A bidirectional switch: transfer control into the guest address space and back.
 *
 * One blob with two entry points, both position independent, both living in the
 * passage page so they stay mapped across the CR3 write.
 *
 *   co_switch_full        save where we are, enter the other address space,
 *                         far-return to the other side's CS:RIP
 *   co_switch_guest_entry where the guest lands: store the sentinel, then run the
 *                         same switch with the two state pointers exchanged,
 *                         which brings the host back
 *
 * Scope is deliberately narrower than a real world switch: only CR3, RSP and
 * CS:RIP are exchanged. Host and guest here share one machine's descriptor
 * tables and MSRs, so restoring them would be writing back identical values --
 * that half is proven separately by co_arch_test_save_restore(), and folding it
 * in would make a failure ambiguous between the two.
 *
 * rcx and rdx carry the two state pointers, and r8/r9 the sentinel address and
 * value. None are touched by the switch, so they survive the far return and are
 * still valid when the guest starts -- which is how the guest knows anything.
 */

/*
 * Offsets inside the passage page's first page, as literals because the fault
 * handler is assembly that has to find them without any register it can trust.
 * Asserted against the real structure below, so they cannot drift.
 */
#define CO_PP_HOST_STATE	"0x740"
#define CO_PP_LINUXVM_STATE	"0x880"
#define CO_PP_FAULTED		"0x9e8"
#define CO_PP_FAULT_RIP		"0x9f0"

#define CO_PP_HOST_STATE_N	0x740
#define CO_PP_LINUXVM_STATE_N	0x880
#define CO_PP_FAULTED_N		0x9e8
#define CO_PP_FAULT_RIP_N	0x9f0

/* params[6] and params[7]; params[8..15] are reserved for the guest GDT. */
#define CO_PP_COUNTER		"0x9f8"
#define CO_PP_REGSUM		"0xa00"
#define CO_PP_COUNTER_N		0x9f8
#define CO_PP_REGSUM_N		0xa00

/*
 * What a fault leaves behind, at params[16..18]. Placed above the GDT's eight
 * reserved slots so a TSS descriptor can be added there without moving these --
 * the stubs reach them by literal offset and cannot be recompiled per layout.
 */
#define CO_PP_VECTOR		"0xa48"
#define CO_PP_ERRCODE		"0xa50"
#define CO_PP_CR2		"0xa58"
#define CO_PP_VECTOR_N		0xa48
#define CO_PP_ERRCODE_N		0xa50
#define CO_PP_CR2_N		0xa58

/*
 * The 256 vector stubs get a page of their own in host_temp, right after the
 * IDT. They have to be executable and mapped in the guest, which the whole
 * passage page already is.
 */
#define CO_PP_IDT_PAGE		 0		/* host_temp page 0 */
#define CO_PP_STUBS_PAGE	 1		/* host_temp page 1 */
#define CO_PP_ISTSTACK_PAGE	 2		/* the stack every fault lands on */
/*
 * Page 3 is deliberately left empty. IST1 is the *top* of the stack page, which
 * is byte zero of the next one, so putting the TSS immediately above the stack
 * would mean a single push past the top silently overwrites it -- the same shape
 * as the bug that put the guest IDT on top of the guest PML4. A page of nothing
 * is cheap; finding that corruption from a triple fault is not.
 */
#define CO_PP_TSS_PAGE		 4
#define CO_PP_CONSOLE_PAGE	 5	/* the early console's ring buffer */
#define CO_PP_FPU_PAGE		 6	/* two 512-byte FXSAVE areas */

/*
 * Extended state, saved on every crossing.
 *
 * The i386 port does not do this, and reading its code I concluded the omission
 * was deliberate delegation to the guest. It was deliberate, but the reason does
 * not carry over: coLinux's changelog shows years of FPU corruption -- crashes
 * in xor_block_pIII_sse and raid6_sse -- ending in a lazy scheme that saves only
 * when the guest actually touches the FPU. That is sound exactly while "the
 * guest never used the FPU" can be true. On x86-64 it never is: the ABI passes
 * floats in XMM and the compiler emits SSE for ordinary memcpy, so the first
 * crossing already has live XMM state on both sides.
 *
 * Corruption here does not crash. It returns wrong numbers to whatever host
 * thread was interrupted, which is the worst failure mode available.
 *
 * FXSAVE rather than XSAVE, and that is a property of this host rather than a
 * shortcut: the measured CR4 is 0x6f8, which has OSFXSR but not OSXSAVE, so
 * nothing on the machine can use AVX and the 512-byte x87+SSE area is the whole
 * of the extended state. A host with OSXSAVE set needs XSAVE against XCR0 --
 * which the state block already carries, for this reason.
 */
#define CO_PP_FPU_HOST		"0xe000"	/* host_temp page 6 + 0x000 */
#define CO_PP_FPU_GUEST		"0xe200"	/* host_temp page 6 + 0x200 */
#define CO_PP_STUB_SIZE		16		/* uniform, so stub N is base + N*16 */
/*
 * params[20..24]: calling a function that the loaded kernel compiled.
 *
 * The kernel is SysV -- arguments in rdi, rsi, rdx -- while the switch leaves
 * rcx, rdx, r8 and r9 set for its own purposes, so the two conventions cannot
 * simply meet. A shim in the passage page loads the arguments from here, pushes
 * a return address, and jumps; the callee returns into that address like any
 * other caller, and the trampoline there switches back.
 */
#define CO_PP_CALL_TARGET	"0xa68"
#define CO_PP_CALL_ARG0		"0xa70"
#define CO_PP_CALL_ARG1		"0xa78"
#define CO_PP_CALL_ARG2		"0xa80"
#define CO_PP_CALL_RET		"0xa88"
#define CO_PP_CALL_ARG3		"0xa90"
#define CO_PP_CALL_ARG4		"0xa98"
#define CO_PP_CALL_ARG5		"0xaa0"

/*
 * params[28]: where the interrupted guest's register frame was left.
 *
 * A guest that is going to be resumed cannot have its registers thrown away,
 * and until the guest kernel has coLinux's own interrupt entry hooks -- which
 * would do SAVE_ALL themselves -- the stub has to preserve them. It pushes the
 * full set onto the IST stack, records rsp here, and the resume path pops them
 * and returns with iretq into the exact instruction that was interrupted.
 */
#define CO_PP_GUEST_FRAME	"0xaa8"
#define CO_PP_GUEST_FRAME_N	0xaa8

/*
 * params[29]: run the guest one instruction at a time.
 *
 * Only three things can take control away from a guest running with interrupts
 * disabled -- an exception, an NMI, or the trap flag. start_kernel disables
 * interrupts almost immediately and leaves them off through the whole of
 * setup_arch, so the interrupt path bounds nothing during exactly the stretch
 * that matters. TF does: the CPU raises #DB after every instruction whether or
 * not IF is set, the guest's own IDT sends that to a stub, and the host gets
 * control back. Slow, and unhangeable, which is the trade worth making while
 * finding out where a kernel dies.
 */
#define CO_PP_STEP		"0xab0"
#define CO_PP_STEP_N		0xab0

/*
 * How many more debug traps the stub may handle by itself before it has to give
 * the host its turn back. params[30].
 *
 * Single-stepping cost one world switch per instruction: two CR3 writes and the
 * TLB flushes that go with them, fxsave and fxrstor, fourteen MSR operations,
 * three descriptor table loads -- 3.6us measured, to execute one instruction.
 * A kernel is not usefully described as running at 280,000 instructions per
 * second.
 *
 * None of that is needed to continue stepping. The trap already lands in the
 * guest's own address space, on the guest's IST stack, in code the guest maps:
 * everything required to re-arm the trap flag and go straight back is right
 * there. So the stub does exactly that, and crosses to the host only when this
 * counter runs out.
 *
 * The counter is what keeps it safe. It is the bound on how long a guest that
 * cannot be interrupted may run before the host is heard from again, which is
 * the same guarantee stepping gave -- just amortised. Anything that is not a
 * debug trap still crosses immediately, so a fault is reported as promptly as
 * before.
 */
#define CO_PP_BATCH		"0xab8"
#define CO_PP_BATCH_N		0xab8

/*
 * A ring of the last sixteen addresses the guest stepped through, written by
 * the stub, params[31..46], with the write index in params[47].
 *
 * The host used to record this itself, once per crossing. With the crossings
 * gone it would otherwise see one address in every few thousand, and "where was
 * it when it stopped" is the question this whole apparatus exists to answer.
 */
#define CO_PP_TRACE_N		0xac0
#define CO_PP_TRACE		"0xac0"
#define CO_PP_TRACE_IDX		"0xb40"
#define CO_PP_TRACE_IDX_N	0xb40
#define CO_PP_CALL_TARGET_N	0xa68
#define CO_PP_CALL_RET_N	0xa88

/*
 * gdt.base, which is two bytes into the ten-byte descriptor (limit first).
 * The blob needs it as a literal; asserted against the struct below.
 */
#define CO_PP_GDT_BASE		"0x62"
#define CO_PP_GDT_BASE_N	0x62

extern char co_switch_full;
extern char co_switch_guest_entry;
extern char co_switch_full_end;

asm(".text                                                          \n"
    ".globl co_switch_full                                          \n"
    "co_switch_full:                                                \n"
    /* rcx = state we are leaving, rdx = state we are entering */
    "    push %rbx                                                  \n"
    "    push %rbp                                                  \n"
    "    push %rdi                                                  \n"
    "    push %rsi                                                  \n"
    "    push %r12                                                  \n"
    "    push %r13                                                  \n"
    "    push %r14                                                  \n"
    "    push %r15                                                  \n"
    "    pushfq                                                     \n"
    "    cli                                                        \n"
    /* record where to resume, and on what stack, in the state we are leaving */
    "    lea 1f(%rip), %rax                                         \n"
    "    mov %rax, " CO_ARCH_STATE_STACK_RETURN_RIP "(%rcx)         \n"
    "    mov %rsp, " CO_ARCH_STATE_STACK_RSP "(%rcx)                \n"
    "    mov %cs, %eax                                              \n"
    "    mov %rax, " CO_ARCH_STATE_STACK_CS "(%rcx)                 \n"
    "    mov %ss, %eax                                              \n"
    "    mov %rax, " CO_ARCH_STATE_STACK_SS "(%rcx)                 \n"
    /*
     * The GDT has to travel with the address space. lretq does not merely set
     * CS:RIP, it loads a segment descriptor -- so it reads the GDT. Leaving GDTR
     * pointing at the host's table across the CR3 write means that read lands on
     * an unmapped page, and the page-fault handler is unmapped too: double fault,
     * triple fault, instant reset with no bugcheck. Learned the hard way.
     */
    /*
     * The leaving side's extended state, into the area belonging to whichever
     * side that is. The state pointer is inside the passage page, so masking it
     * gives the page base; the two blocks sit at fixed offsets, so comparing the
     * low bits says which one this is.
     */
    "    mov %rcx, %rax                                             \n"
    "    and $-4096, %rax                                           \n"
    "    mov %rcx, %r11                                             \n"
    "    and $0xfff, %r11d                                          \n"
    "    cmp $" CO_PP_HOST_STATE ", %r11d                           \n"
    "    je 5f                                                      \n"
    "    lea " CO_PP_FPU_GUEST "(%rax), %rax                        \n"
    "    jmp 6f                                                     \n"
    "5:  lea " CO_PP_FPU_HOST "(%rax), %rax                         \n"
    "6:  fxsave64 (%rax)                                            \n"
    "    sgdt " CO_ARCH_STATE_STACK_GDT "(%rcx)                     \n"
    /*
     * And the IDT with it, for exactly the same reason one step removed. IDTR is
     * not read by the switch itself -- interrupts are off and nothing faults, which
     * is why a round trip passes without this -- but the instant anything does
     * fault in the other address space, the CPU reads a gate from wherever IDTR
     * points. Left pointing at the host's table, that read is unmapped, and so is
     * the page fault handler that would report it: double fault, triple fault,
     * reset. The IDT travels with the address space, just like the GDT.
     */
    "    sidt " CO_ARCH_STATE_STACK_IDT "(%rcx)                     \n"
    "    sldt " CO_ARCH_STATE_STACK_LDT "(%rcx)                     \n"
    "    str " CO_ARCH_STATE_STACK_TR "(%rcx)                       \n"
    /*
     * And the segment-base and syscall MSRs, saved here rather than assumed.
     *
     * The entering side's copies of these are written back further down, and
     * for a long time that was the whole story: they were captured once, by
     * co_arch_save_state() when the passage page was set up, and every crossing
     * afterwards wrote those same values into the host.
     *
     * That is only correct if the host's never change, and the host is Windows.
     * GS_BASE holds the KPCR -- the per-processor block -- and KERNEL_GS_BASE
     * its counterpart; the scheduler updates them, and they differ per
     * processor. So the first time anything moved, every subsequent crossing
     * restored a stale pointer to per-CPU data, and the host carried on with
     * someone else's KPCR. Nothing faults at that moment. It dies later,
     * somewhere unrelated, differently each time -- which is exactly the shape
     * of the failures that took a day to corner.
     *
     * The descriptor tables were always done properly, two instructions above:
     * sgdt/sidt read the live registers on every crossing. These now do the
     * same. The rule is that anything restored per crossing must be saved per
     * crossing.
     *
     * rdmsr returns edx:eax zero-extended into rdx:rax and takes the register
     * number in ecx, all three of which are in use here -- rcx and rdx carry
     * the state pointers -- so both are saved across the sequence. r11 holds
     * the leaving state while they are on the stack.
     */
    "    push %rcx                                                  \n"
    "    push %rdx                                                  \n"
    "    mov %rcx, %r11                                             \n"
#define CO_SAVE_MSR(number, offset)					\
    "    mov $" number ", %ecx                                      \n"	\
    "    rdmsr                                                      \n"	\
    "    shl $32, %rdx                                              \n"	\
    "    or %rdx, %rax                                              \n"	\
    "    mov %rax, " offset "(%r11)                                 \n"
    CO_SAVE_MSR("0xc0000100", CO_ARCH_STATE_FS_BASE)
    CO_SAVE_MSR("0xc0000101", CO_ARCH_STATE_GS_BASE)
    CO_SAVE_MSR("0xc0000102", CO_ARCH_STATE_KERNEL_GS_BASE)
    CO_SAVE_MSR("0xc0000081", CO_ARCH_STATE_STAR)
    CO_SAVE_MSR("0xc0000082", CO_ARCH_STATE_LSTAR)
    CO_SAVE_MSR("0xc0000083", CO_ARCH_STATE_CSTAR)
    CO_SAVE_MSR("0xc0000084", CO_ARCH_STATE_SFMASK)
    CO_SAVE_MSR("0xc0000080", CO_ARCH_STATE_STACK_EFER)
    CO_SAVE_MSR(MSR_IA32_SYSENTER_CS, CO_ARCH_STATE_SYSENTER_CS)
    CO_SAVE_MSR(MSR_IA32_SYSENTER_ESP, CO_ARCH_STATE_SYSENTER_ESP)
    CO_SAVE_MSR(MSR_IA32_SYSENTER_EIP, CO_ARCH_STATE_SYSENTER_EIP)
    /*
     * TSC_AUX (0xC0000103): the auxiliary value RDTSCP returns alongside the
     * timestamp. Windows 10 stores the logical processor number here, and
     * PatchGuard verifies it has not been modified (arg4 = 0x7, "critical MSR
     * modification").
     *
     * The guest's cpu_init() writes its own CPU ID, and the switch never
     * saved or restored it, so the guest's value bled back into Windows.
     * XP through 8.1 either do not use TSC_AUX or do not check it; Win10
     * does both, and the bugcheck fires minutes later on PatchGuard's
     * randomized timer -- confirmed from a minidump with arg3 = 0xC0000103
     * naming the register exactly.
     *
     * Saved into the leaving side's state block at the existing temp_cr3
     * union at offset 0xc0, which is unused during the crossing proper.
     * This avoids growing co_arch_state_stack_t, which would shift every
     * passage-page offset after it and require a guest kernel rebuild.
     */
    "    mov $0xc0000103, %ecx                                      \n"
    "    rdmsr                                                      \n"
    "    shl $32, %rdx                                              \n"
    "    or %rdx, %rax                                              \n"
    "    mov %rax, " CO_ARCH_STATE_STACK_TEMP_CR3 "(%r11)           \n"
    /* XGETBV exists only while this side has CR4.OSXSAVE set. */
    "    mov %cr4, %rax                                             \n"
    "    bt $18, %rax                                               \n"
    "    jnc 9f                                                     \n"
    "    xor %ecx, %ecx                                             \n"
    "    xgetbv                                                     \n"
    "    shl $32, %rdx                                              \n"
    "    or %rdx, %rax                                              \n"
    "    mov %rax, " CO_ARCH_STATE_XCR0 "(%r11)                     \n"
    "9:                                                             \n"
#undef CO_SAVE_MSR
    "    pop %rdx                                                   \n"
    "    pop %rcx                                                   \n"
    /*
     * CR4 and CR0 belong to whichever side is running, and until now they did
     * not travel: CR4 was read once, stashed in r10 across the CR3 write purely
     * to drop global TLB entries, and put back unchanged. That is correct only
     * while neither side writes them -- and the first thing Linux does on entry
     * is write CR4:
     *
     *     movl  $(X86_CR4_PAE|X86_CR4_LA57), %edx
     *     orl   $X86_CR4_MCE, %edx
     *     movq  %cr4, %rcx
     *     andl  %edx, %ecx          <- keeps three bits, drops every other one
     *     btsl  $X86_CR4_PSE_BIT, %ecx
     *     movq  %rcx, %cr4
     *
     * which clears OSFXSR and OSXSAVE. Carried back into Windows that is fatal
     * and not survivably so: every SSE instruction in the kernel starts raising
     * #UD, including the ones in the fault handlers, and the machine stops dead
     * with no bugcheck and no dump. That is exactly what it did -- a freeze that
     * needed the power button, not a reset.
     *
     * So both are saved into the leaving side's state and loaded from the
     * entering side's, like CR3 and the descriptor tables. PGE still has to be
     * cleared before the CR3 write to flush global entries; the entering side's
     * CR4 is written after it, which is also what re-enables PGE if that side
     * had it.
     *
     * CR0 is restored later, after the FXRSTOR, rather than here: FXRSTOR with
     * CR0.TS set raises #NM, and the entering side's TS is not knowable here.
     * Doing it after leaves the FPU sequence running under exactly the CR0 it
     * has always run under.
     */
    /*
     * CR2 and CR3 are live state, not setup constants. In particular, putting
     * back the CR3 captured when the passage page was allocated can resume
     * Windows in a page table belonging to an earlier thread. That faults
     * before the C-side checker is reachable, so it presents as an immediate
     * host freeze with no final diagnostic record.
     */
    "    mov %cr2, %rax                                             \n"
    "    mov %rax, " CO_ARCH_STATE_STACK_CR2 "(%rcx)                \n"
    "    mov %cr3, %rax                                             \n"
    "    mov %rax, " CO_ARCH_STATE_STACK_CR3 "(%rcx)                \n"
    "    mov %cr8, %rax                                             \n"
    "    mov %rax, " CO_ARCH_STATE_CR8 "(%rcx)                       \n"
    "    mov %cr4, %rax                                             \n"
    "    mov %rax, " CO_ARCH_STATE_STACK_CR4 "(%rcx)                \n"
    "    btr $7, %rax                                               \n"
    "    mov %rax, %cr4                                             \n"
    "    mov %cr0, %rax                                             \n"
    "    mov %rax, " CO_ARCH_STATE_STACK_CR0 "(%rcx)                \n"
    /*
     * The leaving side's data segment selectors, saved because the entering
     * side's are restored -- anything restored per crossing must be saved per
     * crossing, which is the rule the MSRs above are here to obey.
     *
     * Read before the CR3 write purely so this sits beside the rest of the
     * save; the state block is in the passage page and is mapped either way.
     */
    "    xor %eax, %eax                                             \n"
    "    mov %ds, %ax                                               \n"
    "    mov %rax, " CO_ARCH_STATE_STACK_DS "(%rcx)                 \n"
    "    mov %es, %ax                                               \n"
    "    mov %rax, " CO_ARCH_STATE_STACK_ES "(%rcx)                 \n"
    "    mov %fs, %ax                                               \n"
    "    mov %rax, " CO_ARCH_STATE_STACK_FS "(%rcx)                 \n"
    "    mov %gs, %ax                                               \n"
    "    mov %rax, " CO_ARCH_STATE_STACK_GS "(%rcx)                 \n"
    /*
     * Debug registers are per-CPU state too. Save all of them, then disarm
     * breakpoints while the passage code runs so one side's watchpoint cannot
     * fire against the other side's address space. DR7 is restored last below.
     */
    "    mov %dr0, %rax                                             \n"
    "    mov %rax, " CO_ARCH_STATE_STACK_DR0 "(%rcx)                \n"
    "    mov %dr1, %rax                                             \n"
    "    mov %rax, " CO_ARCH_STATE_STACK_DR1 "(%rcx)                \n"
    "    mov %dr2, %rax                                             \n"
    "    mov %rax, " CO_ARCH_STATE_STACK_DR2 "(%rcx)                \n"
    "    mov %dr3, %rax                                             \n"
    "    mov %rax, " CO_ARCH_STATE_STACK_DR3 "(%rcx)                \n"
    "    mov %dr6, %rax                                             \n"
    "    mov %rax, " CO_ARCH_STATE_STACK_DR6 "(%rcx)                \n"
    "    mov %dr7, %rax                                             \n"
    "    mov %rax, " CO_ARCH_STATE_STACK_DR7 "(%rcx)                \n"
    "    mov $0x400, %eax                                           \n"
    "    mov %rax, %dr7                                             \n"
    /*
     * --- the crossing ---
     *
     * Uninterruptible, and this is the only place it can be done.
     *
     * The CR3 write below lands several instructions before the lidt, and in
     * that gap the processor is already in the entering side's address space
     * while IDTR still points at the table the leaving side was using. That
     * was survivable for as long as the guest's IDT lived in this page, which
     * is mapped at the same address in both spaces. Once the guest adopted its
     * own IDT -- ordinary guest kernel memory, unmapped on the host side -- an
     * interrupt arriving in the gap made the processor fetch a gate from an
     * unmapped page, with a fault handler equally unreachable: page fault,
     * double fault, triple fault, and an instant reset with no bugcheck and
     * nothing in the log.
     *
     * Only a cli, with nothing saved and nothing restored, because there is
     * nowhere here to put it. RSP still belongs to the leaving side until the
     * stack switch further down, so a push between the CR3 write and that
     * point touches memory the entering address space need not map; and r11,
     * the obvious scratch, is taken by the ltr sequence below.
     *
     * Nothing is lost by not restoring. Each side already owns its own
     * interrupt state on the far side of the crossing: the monitor loop puts
     * the host's saved flags back when the switch returns to it, and the guest
     * re-enables explicitly after its yield (co_colinux_switch in the guest
     * tree), because a guest must always run with real interrupts on.
     *
     * Doing this in the C caller instead was tried and was much worse: the
     * call it would wrap contains the host's entire monitor-loop iteration,
     * so any resume path not returning through the matching restore left the
     * guest running deaf -- a host freeze rather than a reset, and harder to
     * recognise for it.
     */
    "    cli                                                        \n"
    /*
     * CR4 before CR3, and this ordering is not negotiable.
     *
     * CR4.PCIDE (bit 17) controls whether bits 11:0 of CR3 are a PCID and
     * bit 63 is the NOFLUSH flag. When PCIDE is clear those bits are
     * reserved, and writing them raises #GP.
     *
     * Windows 10 with KVA Shadow (KPTI) sets PCIDE and puts a PCID in
     * CR3. The guest clears PCIDE (Linux boots with nopcid here, and
     * init_mem_mapping writes CR4 without it). So the leaving side's CR4
     * has PCIDE off, and the entering host's CR3 has PCID bits set. If CR3
     * is written first -- as it was -- the processor sees reserved bits in
     * a CR3 write and raises #GP. That fires with IF clear, in the passage
     * page, with the guest's IDT still loaded: #GP → #PF (unmapped gate)
     * → #DF → triple fault → machine freeze with no dump.
     *
     * Confirmed on Windows 10 IoT Enterprise LTSC 21H2 (19044.1288):
     * instant CLOCK_WATCHDOG_TIMEOUT freeze on starting the guest, fixed
     * by disabling KVA Shadow (FeatureSettingsOverride = 3). The watchdog
     * is secondary -- every core is dead because a triple fault on one
     * core during the switch means the host IDT is never restored, so
     * every subsequent interrupt on every core hits an unmapped gate.
     *
     * PGE (bit 7) is still cleared, for the same reason it always was:
     * global TLB entries survive a CR3 write and must be flushed. btr $7
     * was already done on the leaving side's CR4 (save path above), but
     * the entering side may also have PGE set, and its CR4 is what we are
     * loading. So clear it, write CR4, write CR3, then put PGE back.
     *
     * The sequence is: load entering CR4 with PGE cleared → write CR3 →
     * restore PGE in CR4. PCIDE is set before CR3 touches it, and PGE is
     * off during the CR3 write that needs the flush.
     */
    "    mov " CO_ARCH_STATE_STACK_CR4 "(%rdx), %rax                \n"
    "    btr $7, %rax                           /* clear PGE */     \n"
    "    mov %rax, %cr4                                             \n"
    "    mov " CO_ARCH_STATE_STACK_CR3 "(%rdx), %rax                \n"
    "    mov %rax, %cr3                                             \n"
    "    mov " CO_ARCH_STATE_STACK_CR4 "(%rdx), %rax                \n"
    "    mov %rax, %cr4                         /* PGE back */      \n"
    "    mov " CO_ARCH_STATE_CR8 "(%rdx), %rax                       \n"
    "    mov %rax, %cr8                                             \n"
    /*
     * Now in the other address space, so its GDT is reachable. This must come
     * after the CR3 write and before the lretq that reads it.
     */
    "    lgdt " CO_ARCH_STATE_STACK_GDT "(%rdx)                     \n"
    "    lidt " CO_ARCH_STATE_STACK_IDT "(%rdx)                     \n"
    "    lldt " CO_ARCH_STATE_STACK_LDT "(%rdx)                     \n"

    /*
     * And SS, which the switch used to leave alone.
     *
     * In long mode SS carries no base or limit, so it looks ignorable -- but
     * the CPU still pushes it when delivering an interrupt, and iretq still
     * reloads it. A guest entered without one keeps the host's SS, and the host
     * selector means something entirely different in the guest's GDT: 0x18 here
     * is the upper half of the TSS descriptor, so the first iretq out of a
     * single-step raised #GP with error code 0x18, naming the selector exactly.
     *
     * The guest is given a null SS, which long mode permits at CPL 0, so there
     * is no descriptor to get wrong. The host gets back the selector it had.
     */
    "    mov " CO_ARCH_STATE_STACK_SS "(%rdx), %eax                  \n"
    "    mov %ax, %ss                                               \n"
    /*
     * And the data segment selectors, for the same reason SS needed doing.
     *
     * These looked ignorable for longer than the others because in long mode
     * DS and ES carry no base and no limit, and FS and GS get their bases from
     * MSRs the switch already carries. So nothing appeared to depend on the
     * selector -- and nothing did, while no guest ever wrote one.
     *
     * __switch_to writes all of them. __loadsegment_ds/es/fs are built to fault
     * and recover:
     *
     *     1: movl %k0,%%es
     *        _ASM_EXTABLE_TYPE_REG(1b, 1b, EX_TYPE_ZERO_REG, %k0)
     *
     * -- if the selector is refused, zero the register and run the instruction
     * again, which loads the null selector and always succeeds. So a guest
     * reaching its first task switch deliberately ends up with ds, es and fs
     * null, and with the selectors not travelling, Windows was handed them.
     *
     * It did not fault at the crossing. It died later in its own dispatcher at
     * SYNCH_LEVEL, on bugcheck 0xA, reading a pointer through state it no
     * longer had -- the same shape as the stale GS_BASE that took a day to
     * corner. Two minidumps name it exactly: the crash before this bug was
     * reachable has ds 002b es 002b fs 0053 gs 002b, and the one after has all
     * four zero.
     *
     * Before the wrmsr block below, and that ordering is the substance of it:
     * writing %fs or %gs in long mode loads the base from the descriptor --
     * zero for a flat one -- so restoring the MSRs first would have them
     * silently undone. co_arch_restore_state() in state.c says the same thing
     * about the same two registers.
     */
    "    mov " CO_ARCH_STATE_STACK_DS "(%rdx), %eax                  \n"
    "    mov %ax, %ds                                               \n"
    "    mov " CO_ARCH_STATE_STACK_ES "(%rdx), %eax                  \n"
    "    mov %ax, %es                                               \n"
    /*
     * FS and GS are loaded only if their descriptors are still loadable, and
     * this is the difference between a working 32-bit userland and a machine
     * that resets.
     *
     * A selector load is a descriptor load: `mov %ax,%gs` reads GDT[index] and
     * faults if that entry is not present or not a data segment. Every other
     * selector this switch restores names a fixed entry -- null, __KERNEL_DS,
     * __USER_DS, __USER32_DS -- and those never change. FS and GS are the
     * exception: a 32-bit process runs with GS = 0x63, which is TLS entry 12,
     * and Linux rewrites entries 12-14 out from under it on every context
     * switch.
     *
     * __switch_to loads the next task's TLS (process_64.c, load_TLS) some
     * distance before it replaces the outgoing GS selector
     * (x86_fsgsbase_load). In between, the GDT holds the next task's
     * descriptors -- all zero for a 64-bit task or a kernel thread -- while the
     * live GS register still holds the outgoing 32-bit task's 0x63. Linux is
     * entitled to that window because __switch_to runs under
     * local_irq_disable(); in this guest that is virtual only (asm/irqflags.h),
     * real interrupts stay on by design, and a host interrupt lands there.
     *
     * The crossing then saves GS = 0x63 faithfully, and re-entry tries to load
     * it against an all-zero descriptor. That #GP cannot be delivered: its gate
     * has no IST, so the frame goes on whatever RSP is current -- and at this
     * point in the switch that is still the host's kernel stack, unmapped in
     * the guest CR3 we have already entered. The resulting #PF escalates to #DF,
     * whose IST lookup needs the TSS through TR, which is still the host's and
     * equally unmapped, because ltr is below. Triple fault: reset, no bugcheck,
     * no dump, nothing in any log.
     *
     * So: null selector, index zero, or a present data descriptor -- load it.
     * Anything else, load null and let the FS_BASE/GS_BASE restore below carry
     * the state. Substituting null is safe precisely in the case that triggers
     * it: the guest is inside __switch_to and is about to load the incoming
     * task's selector itself a few instructions later. An LDT selector is
     * treated the same way rather than validated, because nothing here installs
     * an LDT and a table we do not own is not one to go reading.
     */
    "    mov " CO_ARCH_STATE_STACK_FS "(%rdx), %eax                  \n"
    "    test $0xfff8, %eax                                         \n"
    "    jz 8f                                                      \n"
    "    test $4, %al                  /* TI: an LDT selector */    \n"
    "    jnz 7f                                                     \n"
    "    movzwl %ax, %r11d                                          \n"
    "    and $0xfff8, %r11d                                         \n"
    "    add " CO_PP_GDT_BASE "(%rdx), %r11                         \n"
    "    movzbl 5(%r11), %r11d         /* P, S and type byte */     \n"
    "    and $0x90, %r11d              /* present, non-system */    \n"
    "    cmp $0x90, %r11d                                           \n"
    "    je 8f                                                      \n"
    "7:  xor %eax, %eax                                             \n"
    "8:  mov %ax, %fs                                               \n"
    "    mov " CO_ARCH_STATE_STACK_GS "(%rdx), %eax                  \n"
    "    test $0xfff8, %eax                                         \n"
    "    jz 8f                                                      \n"
    "    test $4, %al                                               \n"
    "    jnz 7f                                                     \n"
    "    movzwl %ax, %r11d                                          \n"
    "    and $0xfff8, %r11d                                         \n"
    "    add " CO_PP_GDT_BASE "(%rdx), %r11                         \n"
    "    movzbl 5(%r11), %r11d                                      \n"
    "    and $0x90, %r11d                                           \n"
    "    cmp $0x90, %r11d                                           \n"
    "    je 8f                                                      \n"
    "7:  xor %eax, %eax                                             \n"
    "8:  mov %ax, %gs                                               \n"
    /*
     * And the task register, so the entering side has a TSS -- which is what
     * makes IST work, and IST is what lets a fault be handled when the current
     * stack is the thing that is broken. Without it a bad RSP is a double fault
     * before any handler runs.
     *
     * The busy bit has to be cleared first, every time. The CPU sets it on ltr
     * and refuses to load a descriptor that already has it, so this is needed
     * both for the host (Windows marked its own busy at boot) and for the guest
     * from the second entry onwards. The write goes into the entering side's
     * live GDT, which is reachable because the CR3 write has already happened
     * and that table is by definition mapped in the space we just entered.
     *
     * rax and r11 only: rcx, rdx, r8 and r9 carry state across the far return.
     */
    "    movzwl " CO_ARCH_STATE_STACK_TR "(%rdx), %r11d             \n"
    "    test %r11d, %r11d                                          \n"
    "    jz 3f                                                      \n"
    "    mov " CO_PP_GDT_BASE "(%rdx), %rax                         \n"
    "    and $-8, %r11d                                             \n"
    "    add %r11, %rax                                             \n"
    "    andl $0xfffffdff, 4(%rax)     /* clear the busy bit */     \n"
    "    ltr " CO_ARCH_STATE_STACK_TR "(%rdx)                       \n"
    /*
     * And clear it again -- but ONLY when the side being entered is the
     * guest. Never in Windows' GDT.
     *
     * The window this closes is the guest's: it writes its TSS descriptor and
     * is about to ltr when a hardware interrupt lands between the two. The
     * crossing re-enters through this path, this ltr marks the descriptor
     * busy, and the guest resumes at its own ltr -- which the CPU refuses with
     * #GP(TSS selector), error code 0x40. Boot dies seconds in, only when an
     * interrupt hits an instructions-wide window, which is why it presented as
     * a coin-flip weighted by how busy the host was.
     *
     * Doing it unconditionally is what the first version did, and it left the
     * HOST's TSS descriptor permanently marked available instead of busy --
     * a standing modification to a live processor GDT. XP x64 is the first
     * Windows with Kernel Patch Protection, and a modified GDT is precisely
     * what it checks for: bugcheck 0x109 CRITICAL_STRUCTURE_CORRUPTION with
     * parameter 4 = 3 ("a processor GDT") and parameter 3 pointing at the
     * host's own GDT base. Because the check runs on a timer, the machine died
     * minutes later, with the box idle and nothing of ours on the stack --
     * which is why it read as a spontaneous, unrelated crash. This is the same
     * reason arch/i386's antinx has no successor here: the host's structures
     * are not ours to edit.
     *
     * Leaving the GUEST's descriptor available is safe and is what its own ltr
     * needs: TR caches the descriptor on load and never re-reads memory, long
     * mode has no hardware task switching, so the only consumer of the busy
     * bit is ltr itself -- the one instruction that wants it clear. And the
     * guest's GDT is the guest's to keep consistent.
     *
     * rdx is the entering side's state block inside the page-aligned passage
     * page, so its low twelve bits identify the side -- the same test the
     * extended-state save uses above.
     */
    "    mov %rdx, %r11                                             \n"
    "    and $0xfff, %r11d                                          \n"
    "    cmp $" CO_PP_LINUXVM_STATE ", %r11d                        \n"
    "    jne 3f                                                     \n"
    "    andl $0xfffffdff, 4(%rax)                                  \n"
    "3:                                                             \n"
    /* onto the other side's stack, then far-return to its cs:rip */
    "    mov " CO_ARCH_STATE_STACK_RSP "(%rdx), %rsp                \n"
    /*
     * Put the entering side's segment-base and syscall MSRs back.
     *
     * These are per-CPU registers, not per-address-space, so a guest that
     * writes one leaves it written for whoever runs next -- and Windows keeps
     * the KPCR in GS_BASE. cpu_init() writes GS_BASE during early boot, so a
     * guest that gets that far would hand the host back a null KPCR pointer and
     * kill it on the next interrupt. LSTAR is the same shape: the guest could
     * leave host syscalls pointing into guest code.
     *
     * wrmsr wants the value in edx:eax and the register number in ecx, which
     * are the two registers carrying the state pointers, so both are saved
     * across the sequence. The stack is the entering side's and is already
     * valid here.
     *
     * EFER is deliberately not restored: LMA is read-only and writing it back
     * raises #GP on some parts, so it needs masking, and a guest that had
     * corrupted LME would have triple faulted before reaching here anyway.
     */
    "    push %rcx                                                  \n"
    "    push %rdx                                                  \n"
    "    mov %rdx, %r11                                             \n"
    "    mov $0xc0000100, %ecx      /* FS_BASE */                   \n"
    "    mov " CO_ARCH_STATE_FS_BASE "(%r11), %rax                  \n"
    "    mov %rax, %rdx                                             \n"
    "    shr $32, %rdx                                              \n"
    "    wrmsr                                                      \n"
    "    mov $0xc0000101, %ecx      /* GS_BASE -- the KPCR */       \n"
    "    mov " CO_ARCH_STATE_GS_BASE "(%r11), %rax                  \n"
    "    mov %rax, %rdx                                             \n"
    "    shr $32, %rdx                                              \n"
    "    wrmsr                                                      \n"
    "    mov $0xc0000102, %ecx      /* KERNEL_GS_BASE */            \n"
    "    mov " CO_ARCH_STATE_KERNEL_GS_BASE "(%r11), %rax           \n"
    "    mov %rax, %rdx                                             \n"
    "    shr $32, %rdx                                              \n"
    "    wrmsr                                                      \n"
    "    mov $0xc0000081, %ecx      /* STAR */                      \n"
    "    mov " CO_ARCH_STATE_STAR "(%r11), %rax                     \n"
    "    mov %rax, %rdx                                             \n"
    "    shr $32, %rdx                                              \n"
    "    wrmsr                                                      \n"
    "    mov $0xc0000082, %ecx      /* LSTAR */                     \n"
    "    mov " CO_ARCH_STATE_LSTAR "(%r11), %rax                    \n"
    "    mov %rax, %rdx                                             \n"
    "    shr $32, %rdx                                              \n"
    "    wrmsr                                                      \n"
    "    mov $0xc0000083, %ecx      /* CSTAR */                     \n"
    "    mov " CO_ARCH_STATE_CSTAR "(%r11), %rax                    \n"
    "    mov %rax, %rdx                                             \n"
    "    shr $32, %rdx                                              \n"
    "    wrmsr                                                      \n"
    "    mov $0xc0000084, %ecx      /* SFMASK */                    \n"
    "    mov " CO_ARCH_STATE_SFMASK "(%r11), %rax                   \n"
    "    mov %rax, %rdx                                             \n"
    "    shr $32, %rdx                                              \n"
    "    wrmsr                                                      \n"
    /* LMA is read-only; preserve it by omitting it from the WRMSR value. */
    "    mov $0xc0000080, %ecx      /* EFER */                      \n"
    "    mov " CO_ARCH_STATE_STACK_EFER "(%r11), %rax               \n"
    "    btr $10, %rax                                              \n"
    "    mov %rax, %rdx                                             \n"
    "    shr $32, %rdx                                              \n"
    "    wrmsr                                                      \n"
    "    mov $" MSR_IA32_SYSENTER_CS ", %ecx /* SYSENTER_CS */      \n"
    "    mov " CO_ARCH_STATE_SYSENTER_CS "(%r11), %rax              \n"
    "    mov %rax, %rdx                                             \n"
    "    shr $32, %rdx                                              \n"
    "    wrmsr                                                      \n"
    "    mov $" MSR_IA32_SYSENTER_ESP ", %ecx /* SYSENTER_ESP */    \n"
    "    mov " CO_ARCH_STATE_SYSENTER_ESP "(%r11), %rax             \n"
    "    mov %rax, %rdx                                             \n"
    "    shr $32, %rdx                                              \n"
    "    wrmsr                                                      \n"
    "    mov $" MSR_IA32_SYSENTER_EIP ", %ecx /* SYSENTER_EIP */    \n"
    "    mov " CO_ARCH_STATE_SYSENTER_EIP "(%r11), %rax             \n"
    "    mov %rax, %rdx                                             \n"
    "    shr $32, %rdx                                              \n"
    "    wrmsr                                                      \n"
    "    mov $0xc0000103, %ecx         /* TSC_AUX */                \n"
    "    mov " CO_ARCH_STATE_STACK_TEMP_CR3 "(%r11), %rax           \n"
    "    mov %rax, %rdx                                             \n"
    "    shr $32, %rdx                                              \n"
    "    wrmsr                                                      \n"
    /* XSETBV is legal only when the entering side enables OSXSAVE. */
    "    mov %cr4, %rax                                             \n"
    "    bt $18, %rax                                               \n"
    "    jnc 9f                                                     \n"
    "    mov " CO_ARCH_STATE_XCR0 "(%r11), %rax                     \n"
    "    mov %rax, %rdx                                             \n"
    "    shr $32, %rdx                                              \n"
    "    xor %ecx, %ecx                                             \n"
    "    xsetbv                                                     \n"
    "9:                                                             \n"
    "    pop %rdx                                                   \n"
    "    pop %rcx                                                   \n"
    /* and the entering side's extended state, the mirror of the save above */
    "    mov %rdx, %rax                                             \n"
    "    and $-4096, %rax                                           \n"
    "    mov %rdx, %r11                                             \n"
    "    and $0xfff, %r11d                                          \n"
    "    cmp $" CO_PP_HOST_STATE ", %r11d                           \n"
    "    je 7f                                                      \n"
    "    lea " CO_PP_FPU_GUEST "(%rax), %rax                        \n"
    "    jmp 8f                                                     \n"
    "7:  lea " CO_PP_FPU_HOST "(%rax), %rax                         \n"
    "8:  fxrstor64 (%rax)                                           \n"
    /*
     * CR0 last, once the FPU state is in place -- see the note at the CR4 swap.
     * Linux writes CR0_STATE a few instructions after its CR4 write, and TS is
     * the bit that differs between the two sides, so this is the register that
     * decides whether Windows' lazy-FPU bookkeeping still means anything.
     */
    "    mov " CO_ARCH_STATE_STACK_CR0 "(%rdx), %rax                \n"
    "    mov %rax, %cr0                                             \n"
    "    mov " CO_ARCH_STATE_STACK_CR2 "(%rdx), %rax                \n"
    "    mov %rax, %cr2                                             \n"
    "    push " CO_ARCH_STATE_STACK_CS "(%rdx)                      \n"
    "    push " CO_ARCH_STATE_STACK_RETURN_RIP "(%rdx)              \n"
    /* DR7 last: it arms the address registers. */
    "    mov " CO_ARCH_STATE_STACK_DR0 "(%rdx), %rax                \n"
    "    mov %rax, %dr0                                             \n"
    "    mov " CO_ARCH_STATE_STACK_DR1 "(%rdx), %rax                \n"
    "    mov %rax, %dr1                                             \n"
    "    mov " CO_ARCH_STATE_STACK_DR2 "(%rdx), %rax                \n"
    "    mov %rax, %dr2                                             \n"
    "    mov " CO_ARCH_STATE_STACK_DR3 "(%rdx), %rax                \n"
    "    mov %rax, %dr3                                             \n"
    "    mov " CO_ARCH_STATE_STACK_DR6 "(%rdx), %rax                \n"
    "    mov %rax, %dr6                                             \n"
    "    mov " CO_ARCH_STATE_STACK_DR7 "(%rdx), %rax                \n"
    "    mov %rax, %dr7                                             \n"
    "    lretq                                                      \n"
    /* the other side switching back lands here, on our own stack again */
    "1:  popfq                                                      \n"
    "    pop %r15                                                   \n"
    "    pop %r14                                                   \n"
    "    pop %r13                                                   \n"
    "    pop %r12                                                   \n"
    "    pop %rsi                                                   \n"
    "    pop %rdi                                                   \n"
    "    pop %rbp                                                   \n"
    "    pop %rbx                                                   \n"
    "    ret                                                        \n"
    ".globl co_switch_guest_entry                                   \n"
    "co_switch_guest_entry:                                         \n"
    /* running in the guest address space now, on the guest stack */
    "    mov %r9, (%r8)                                             \n"
    /* swap the roles and go back the way we came */
    "    xchg %rcx, %rdx                                            \n"
    "    jmp co_switch_full                                         \n"
    /*
     * Where a guest exception lands.
     *
     * Reached through the guest's own IDT, so it runs in the guest address space
     * and must live in the passage page. Nothing in any register can be trusted
     * -- the faulting code owned them -- so it recovers the passage page's base
     * from its own RIP, which works because this code is in the first page and
     * the page is 4 KB aligned.
     *
     * Then it records the fault and re-enters the switch with the guest as the
     * side being left. The host resumes exactly where it would have on a normal
     * return, and inspects the flag.
     *
     * This is interrupt forwarding in miniature, and the same shape the real one
     * takes: the guest catches, records, and hands control back rather than
     * trying to handle anything itself.
     */
    /*
     * An alternate guest entry that stores the sentinel and then deliberately
     * raises #UD. If the guest IDT works, this comes back through the handler
     * with the fault recorded; if it does not, it is a triple fault and a reset.
     * That is the whole difference being tested.
     */
    ".globl co_switch_guest_entry_fault                             \n"
    "co_switch_guest_entry_fault:                                   \n"
    "    mov %r9, (%r8)                                             \n"
    "    ud2                                                        \n"
    /*
     * Every vector arrives here, but by way of its own stub, which has already
     * pushed two words so the frame is the same shape whatever faulted:
     *
     *   rsp+0x00  vector number      (pushed by the stub)
     *   rsp+0x08  error code         (real, or a zero the stub pushed)
     *   rsp+0x10  RIP                    <- CPU
     *   rsp+0x18  CS                     <- CPU
     *   rsp+0x20  RFLAGS                 <- CPU
     *   rsp+0x28  RSP                    <- CPU
     *   rsp+0x30  SS                     <- CPU
     *
     * Normalising the error code in the stub is the whole reason the stubs
     * exist. Ten vectors push one and the rest do not, so without it the frame
     * is shifted by eight bytes for exactly the faults that matter most -- #PF
     * and #GP -- and the RIP read here would be the CS of a different fault.
     *
     * Registers are clobbered freely: a fault ends the guest's turn, so there is
     * nothing to preserve. If a later rung wants to resume after a fault, this
     * has to save the full register set first.
     */
    ".globl co_switch_guest_fault                                   \n"
    "co_switch_guest_fault:                                         \n"
    /*
     * Save everything before touching anything. The guest may be resumed, and
     * an interrupt arrives at an arbitrary instruction, so every register is
     * live. Fifteen pushes: rsp itself is already in the interrupt frame.
     *
     * Frame from rsp after this, which the offsets below depend on:
     *   0x00 r15  0x08 r14  0x10 r13  0x18 r12  0x20 r11  0x28 r10
     *   0x30 r9   0x38 r8   0x40 rbp  0x48 rdi  0x50 rsi  0x58 rdx
     *   0x60 rcx  0x68 rbx  0x70 rax
     *   0x78 vector  0x80 error code   (pushed by the stub)
     *   0x88 RIP  0x90 CS  0x98 RFLAGS  0xa0 RSP  0xa8 SS   (pushed by the CPU)
     */
    "    push %rax                                                  \n"
    "    push %rbx                                                  \n"
    "    push %rcx                                                  \n"
    "    push %rdx                                                  \n"
    "    push %rsi                                                  \n"
    "    push %rdi                                                  \n"
    "    push %rbp                                                  \n"
    "    push %r8                                                   \n"
    "    push %r9                                                   \n"
    "    push %r10                                                  \n"
    "    push %r11                                                  \n"
    "    push %r12                                                  \n"
    "    push %r13                                                  \n"
    "    push %r14                                                  \n"
    "    push %r15                                                  \n"
    "    lea 0(%rip), %rax                                          \n"
    "    and $-4096, %rax                                           \n"
    /*
     * A debug trap with budget left never leaves the guest.
     *
     * Record where it was, put the trap flag back on and interrupts back off in
     * the frame about to be returned through, and iretq. About twenty
     * instructions, all of them in the guest's own address space, instead of a
     * round trip through the host.
     *
     * Only #DB is treated this way. Everything else -- a fault, or an interrupt
     * that got through -- falls into the crossing below and is reported at once,
     * so nothing is hidden by batching; only stepping is made affordable.
     *
     * rax holds the passage page, rdx is free (saved above), and the frame is
     * at a known offset from rsp because this stub pushed it.
     */
    "    cmpq $1, 0x78(%rsp)                                        \n"
    "    jne 9f                                                     \n"
    "    cmpq $0, " CO_PP_BATCH "(%rax)                             \n"
    "    je 9f                                                      \n"
    "    decq " CO_PP_BATCH "(%rax)                                 \n"
    /* trace[idx++ & 15] = the address that just executed */
    "    mov " CO_PP_TRACE_IDX "(%rax), %rdx                        \n"
    "    mov %rdx, %rcx                                             \n"
    "    inc %rdx                                                   \n"
    "    mov %rdx, " CO_PP_TRACE_IDX "(%rax)                        \n"
    "    and $15, %rcx                                              \n"
    "    mov 0x88(%rsp), %rdx                                       \n"
    "    mov %rdx, " CO_PP_TRACE "(%rax,%rcx,8)                     \n"
    /*
     * TF on and IF off for the next instruction, for the same reasons the host
     * did it: the guest may clear either, and neither may be left to it.
     */
    "    orq $0x100, 0x98(%rsp)                                     \n"
    "    andq $-513, 0x98(%rsp)     /* ~0x200: IF */                \n"
    "    pop %r15                                                   \n"
    "    pop %r14                                                   \n"
    "    pop %r13                                                   \n"
    "    pop %r12                                                   \n"
    "    pop %r11                                                   \n"
    "    pop %r10                                                   \n"
    "    pop %r9                                                    \n"
    "    pop %r8                                                    \n"
    "    pop %rbp                                                   \n"
    "    pop %rdi                                                   \n"
    "    pop %rsi                                                   \n"
    "    pop %rdx                                                   \n"
    "    pop %rcx                                                   \n"
    "    pop %rbx                                                   \n"
    "    pop %rax                                                   \n"
    "    add $0x10, %rsp            /* vector and error code */     \n"
    "    iretq                                                      \n"
    "9:                                                             \n"
    "    mov %rsp, " CO_PP_GUEST_FRAME "(%rax)                      \n"
    "    movq $1, " CO_PP_FAULTED "(%rax)                           \n"
    "    mov 0x78(%rsp), %rdx                                       \n"
    "    mov %rdx, " CO_PP_VECTOR "(%rax)                           \n"
    "    mov 0x80(%rsp), %rdx                                       \n"
    "    mov %rdx, " CO_PP_ERRCODE "(%rax)                          \n"
    "    mov 0x88(%rsp), %rdx                                       \n"
    "    mov %rdx, " CO_PP_FAULT_RIP "(%rax)                        \n"
    /* CR2 is only meaningful for #PF, but it costs nothing and is the whole
     * diagnosis when it is: the address that could not be translated. */
    "    mov %cr2, %rdx                                             \n"
    "    mov %rdx, " CO_PP_CR2 "(%rax)                              \n"
    "    lea " CO_PP_LINUXVM_STATE "(%rax), %rcx                    \n"
    "    lea " CO_PP_HOST_STATE "(%rax), %rdx                       \n"
    "    jmp co_switch_full                                         \n"
    /*
     * A guest that takes a page fault on purpose: it computes an address inside
     * its own passage page, walks a megabyte past it -- which the guest maps
     * nothing at -- and reads. Position independent, and guaranteed unmapped
     * without needing to know where the page landed.
     */
    /*
     * A guest that destroys its own stack pointer and then faults.
     *
     * Without IST this is unsurvivable: delivering the #UD means pushing an
     * interrupt frame, pushing it means touching RSP, RSP points at nothing
     * mapped, so the fault handler faults -- double fault, triple fault, reset.
     * With IST1 on every gate the CPU loads RSP from the TSS before it pushes
     * anything, so the garbage RSP is never touched and the fault is delivered
     * normally. Coming back at all is the proof; there is no other mechanism at
     * the same privilege level that could have supplied a stack.
     *
     * The sentinel is stored first, while the stack is still good, so a failure
     * here cannot be confused with never having entered.
     */
    ".globl co_switch_guest_entry_badstack                          \n"
    "co_switch_guest_entry_badstack:                                \n"
    "    mov %r9, (%r8)                                             \n"
    "    lea 0(%rip), %rsp                                          \n"
    "    and $-4096, %rsp                                           \n"
    "    add $0x200000, %rsp        /* 2 MB out: nothing mapped */  \n"
    "    ud2                                                        \n"
    ".globl co_switch_guest_entry_pf                                \n"
    "co_switch_guest_entry_pf:                                      \n"
    "    mov %r9, (%r8)                                             \n"
    "    lea 0(%rip), %rdx                                          \n"
    "    and $-4096, %rdx                                           \n"
    "    add $0x100000, %rdx                                        \n"
    "    mov (%rdx), %rax                                           \n"
    "    ud2                                                        \n"
    /*
     * A guest that yields and expects to be resumed -- the shape the real
     * monitor loop has, reduced to something with no Linux in it.
     *
     * Each time round it bumps a counter in the passage page, then calls the
     * switch to hand control back to the host. Because it *calls* rather than
     * jumps, there is a return address on the guest stack, so when the host
     * re-enters, the switch's epilogue returns here and the loop continues from
     * where it stopped. That is the whole property being tested: the first entry
     * arrives at the top, every later one resumes inside co_switch_full.
     *
     * rbx is zeroed once, above the loop label, and incremented inside it. It is
     * callee-saved, so it lives on the guest stack across each yield -- if it
     * still counts correctly after N crossings, guest registers genuinely
     * survive being switched away from and back to.
     *
     * Nothing is assumed about register contents on resume: rax, rcx and rdx are
     * all recomputed from RIP each iteration, exactly as the fault handler does.
     */
    ".globl co_switch_guest_loop                                    \n"
    "co_switch_guest_loop:                                          \n"
    "    xor %rbx, %rbx               /* first entry only */        \n"
    "2:  lea 0(%rip), %rax                                          \n"
    "    and $-4096, %rax                                           \n"
    "    incq " CO_PP_COUNTER "(%rax)                               \n"
    "    inc %rbx                                                   \n"
    "    mov %rbx, " CO_PP_REGSUM "(%rax)                           \n"
    "    lea " CO_PP_LINUXVM_STATE "(%rax), %rcx                    \n"
    "    lea " CO_PP_HOST_STATE "(%rax), %rdx                       \n"
    "    call co_switch_full                                        \n"
    "    jmp 2b                                                     \n"
    /*
     * Call into code the kernel compiled, and come back.
     *
     * The shim runs in the passage page, so it can find itself by RIP. It sets
     * up a SysV call, pushes the trampoline below as the return address, and
     * jumps. Nothing about the callee is assumed except that it returns -- the
     * unpatched return thunk in this build is a plain ret, so it does.
     *
     * The trampoline cannot rely on r8 still pointing anywhere useful: r8 is
     * caller-saved in SysV and the callee may have used it. It recovers the
     * passage page from its own RIP instead, exactly as the fault handler does.
     */
    /*
     * Put an interrupted guest back exactly where it was.
     *
     * The host sets return_rip here after handing the interrupt to Windows. rsp
     * on arrival is irrelevant -- it is replaced immediately by the frame the
     * stub recorded -- and the iretq restores RIP, CS, RFLAGS, RSP and SS
     * together, so the guest resumes at the interrupted instruction with its
     * interrupt flag as it was.
     */
    ".globl co_guest_resume                                         \n"
    "co_guest_resume:                                               \n"
    "    lea 0(%rip), %rax                                          \n"
    "    and $-4096, %rax                                           \n"
    "    mov " CO_PP_GUEST_FRAME "(%rax), %rsp                      \n"
    "    pop %r15                                                   \n"
    "    pop %r14                                                   \n"
    "    pop %r13                                                   \n"
    "    pop %r12                                                   \n"
    "    pop %r11                                                   \n"
    "    pop %r10                                                   \n"
    "    pop %r9                                                    \n"
    "    pop %r8                                                    \n"
    "    pop %rbp                                                   \n"
    "    pop %rdi                                                   \n"
    "    pop %rsi                                                   \n"
    "    pop %rdx                                                   \n"
    "    pop %rcx                                                   \n"
    "    pop %rbx                                                   \n"
    "    pop %rax                                                   \n"
    "    add $16, %rsp              /* vector and error code */     \n"
    "    iretq                                                      \n"
    /*
     * Entry for booting: enable interrupts, then jump to the kernel.
     *
     * The switch reaches the guest by lretq, which does not restore RFLAGS, so
     * a guest entered any other way runs with IF clear. That is fine for code
     * that returns in microseconds and fatal for code that does not: a Windows
     * thread spinning with interrupts off cannot be preempted, and the other
     * cores eventually trip the clock watchdog and bugcheck the machine. A
     * kernel booting is the first guest that runs long enough to matter.
     */
    ".globl co_boot_shim                                            \n"
    "co_boot_shim:                                                  \n"
    "    lea 0(%rip), %rax                                          \n"
    "    and $-4096, %rax                                           \n"
    "    mov " CO_PP_CALL_TARGET "(%rax), %r10                      \n"
    /*
     * Free-running: real IF goes ON here and stays on for the life of the
     * guest. Hardware interrupts vector through the guest IDT into the stub,
     * cross back to the host, and are replayed into Windows' live IDT -- so
     * the host is never deaf for longer than one crossing, whatever the guest
     * does. The guest's own cli/sti are virtual (asm/irqflags.h in the guest
     * tree) and never touch the real flag; the one raw popfq on the guest's
     * boot path (head_64.S, zero-EFLAGS before initial_code) is guarded to
     * preserve IF for a cooperative guest.
     *
     * The previous policy -- IF clear, voluntary yields at virtual-sti,
     * console and idle boundaries -- froze the machine the moment the guest
     * ran free: early boot runs for hundreds of milliseconds between such
     * boundaries, and a core deaf that long misses the clock and wedges every
     * other core on TLB-shootdown IPIs.
     *
     * Stepping keeps IF clear and uses TF as its per-instruction bound, as
     * before: interrupts are then delivered in host context between
     * crossings, and the forwarding path stays out of the picture.
     */
    "    cmpq $0, " CO_PP_STEP "(%rax)                              \n"
    "    je 4f                                                      \n"
    "    pushfq                                                     \n"
    "    orq $0x100, (%rsp)                                         \n"
    "    popfq                                                      \n"
    "    jmp 5f                                                     \n"
    "4:  sti                                                        \n"
    "5:  jmp *%r10                                                  \n"
    ".globl co_call_shim                                            \n"
    "co_call_shim:                                                  \n"
    "    lea 0(%rip), %rax                                          \n"
    "    and $-4096, %rax                                           \n"
    "    mov " CO_PP_CALL_ARG0 "(%rax), %rdi                        \n"
    "    mov " CO_PP_CALL_ARG1 "(%rax), %rsi                        \n"
    "    mov " CO_PP_CALL_ARG2 "(%rax), %rdx                        \n"
    "    mov " CO_PP_CALL_ARG3 "(%rax), %rcx                        \n"
    "    mov " CO_PP_CALL_TARGET "(%rax), %r10                      \n"
    "    lea co_call_return(%rip), %r11                             \n"
    "    push %r11                  /* the callee's return address */\n"
    /*
     * r8 and r9 last, because rax is still the passage page pointer until now
     * and r8 is where the fourth argument goes.
     */
    "    mov " CO_PP_CALL_ARG4 "(%rax), %r8                         \n"
    "    mov " CO_PP_CALL_ARG5 "(%rax), %r9                         \n"
    /*
     * SysV requires al to hold the number of vector registers used when calling
     * a variadic function. snprintf is variadic and reads it; leaving whatever
     * happened to be in rax makes it save up to eight XMM registers to a stack
     * area we never reserved.
     */
    "    xor %eax, %eax                                             \n"
    "    jmp *%r10                                                  \n"
    ".globl co_call_return                                          \n"
    "co_call_return:                                                \n"
    "    mov %rax, %r11             /* the function's return value */\n"
    "    lea 0(%rip), %rax                                          \n"
    "    and $-4096, %rax                                           \n"
    "    mov %r11, " CO_PP_CALL_RET "(%rax)                         \n"
    "    lea " CO_PP_LINUXVM_STATE "(%rax), %rcx                    \n"
    "    lea " CO_PP_HOST_STATE "(%rax), %rdx                       \n"
    "    jmp co_switch_full                                         \n"
    ".globl co_switch_full_end                                      \n"
    "co_switch_full_end:                                            \n");

extern char co_call_shim;
extern char co_guest_resume;
extern char co_boot_shim;

/*
 * The guest for R4, which does not live in the passage page.
 *
 * Everything before this has executed from inside the passage page, so it could
 * find its own bearings with `lea 0(%rip); and $-4096`. This one runs from a
 * page mapped at a Linux-like address, several terabytes away, and neither
 * trick works there: the RIP is in the wrong page, and a relative jmp cannot
 * span the distance -- rel32 reaches +-2 GB and the gap is about 4.5 TB.
 *
 * So it navigates by r8, which the switch leaves pointing at params[0] inside
 * the passage page. The address of the switch itself is left in params[19], and
 * the jump is indirect. That is exactly how a real guest kernel will have to do
 * it, since it is linked at its own address and knows the passage page only as
 * a value it was handed.
 */
extern char co_extern_guest_code;
extern char co_extern_guest_code_end;
extern char co_extern_guest_fault_code;
extern char co_extern_guest_fault_code_end;

asm(".text                                                          \n"
    ".globl co_extern_guest_code                                    \n"
    "co_extern_guest_code:                                          \n"
    "    mov %r9, (%r8)          /* sentinel, in the passage page */\n"
    "    xchg %rcx, %rdx         /* swap leaving/entering */        \n"
    "    mov 0x98(%r8), %rax     /* params[19]: switch entry */     \n"
    "    jmp *%rax                                                  \n"
    ".globl co_extern_guest_code_end                                \n"
    "co_extern_guest_code_end:                                      \n"
    /* the same, but faulting instead of returning */
    ".globl co_extern_guest_fault_code                              \n"
    "co_extern_guest_fault_code:                                    \n"
    "    mov %r9, (%r8)                                             \n"
    "    ud2                                                        \n"
    ".globl co_extern_guest_fault_code_end                          \n"
    "co_extern_guest_fault_code_end:                                \n");

/* params[19]: where the guest finds the switch. Reached as 0x98(%r8). */
#define CO_PP_SWITCH_ENTRY_N	0xa60


extern char co_switch_guest_fault;
extern char co_switch_guest_entry_fault;
extern char co_switch_guest_loop;
extern char co_switch_guest_entry_pf;
extern char co_switch_guest_entry_badstack;

typedef void (*co_switch_full_fn)(co_arch_state_stack_t* leaving,
                                  co_arch_state_stack_t* entering,
                                  unsigned long long* sentinel,
                                  unsigned long long value);

typedef void (*co_switch_probe_fn)(unsigned long long host_cr3,
				   unsigned long long guest_cr3,
				   unsigned long long* sentinel,
				   unsigned long long value);

#define CO_SWITCH_SENTINEL 0x5741544348454421ULL	/* "SWATCHE!" */

/*
 * A guest address space containing one mapping: the passage page, at the address
 * the host already has it. Same address on both sides is what makes the switch
 * simple -- there is no other_map delta and nothing has to relocate itself.
 */
static bool_t co_build_guest_tables(co_arch_passage_page_t* pp, unsigned long long va)
{
	unsigned long long last_va = va + sizeof(*pp) - 1;
	co_pa_t pd_pa, pdpt_pa;
	int pages = (int)(sizeof(*pp) / CO_ARCH_PAGE_SIZE);
	int page;

	/*
	 * The allocation is fifteen pages, so it spans at most two 2 MB regions and
	 * needs at most two page tables. guest_temp carries pt[2] for exactly that.
	 *
	 * An earlier version installed one PT and refused any allocation that
	 * straddled a 2 MB boundary. The refusal was right -- the alternative was
	 * CO_ARCH_PTE_INDEX wrapping and the tail pages being mapped nowhere -- but
	 * refusing turned out to fire on roughly one run in five, because
	 * NonPagedPool has no reason to keep a 60 KB allocation inside one region.
	 * That is a limitation to remove, not a hazard to guard against.
	 *
	 * Anything beyond two regions, or crossing a 1 GB or 512 GB boundary, is
	 * still refused: it cannot happen for fifteen pages, and silently handling
	 * a case that cannot arise is how the next size change breaks quietly.
	 */
	if (CO_ARCH_PUD_INDEX(va) != CO_ARCH_PUD_INDEX(last_va) ||
	    CO_ARCH_PGD_INDEX(va) != CO_ARCH_PGD_INDEX(last_va)) {
		co_debug_error("passage page 0x%llx..0x%llx crosses a 1GB boundary",
			       va, last_va);
		return PFALSE;
	}

	if (CO_ARCH_PMD_INDEX(last_va) - CO_ARCH_PMD_INDEX(va) > 1) {
		co_debug_error("passage page 0x%llx..0x%llx spans more than two 2MB regions",
			       va, last_va);
		return PFALSE;
	}

	co_memset(&pp->guest_temp, 0, sizeof(pp->guest_temp));

	pd_pa   = co_os_virt_to_phys(&pp->guest_temp.pd[0]);
	pdpt_pa = co_os_virt_to_phys(&pp->guest_temp.pdpt[0]);

	/*
	 * Each page goes into the table for its own 2 MB region: pt[0] for the
	 * region the allocation starts in, pt[1] for the next one if it reaches
	 * that far. NonPagedPool is not physically contiguous, so every page is
	 * looked up individually.
	 */
	for (page = 0; page < pages; page++) {
		unsigned char* p = (unsigned char*)pp + page * CO_ARCH_PAGE_SIZE;
		unsigned long long page_va = va + page * CO_ARCH_PAGE_SIZE;
		int which = (CO_ARCH_PMD_INDEX(page_va) == CO_ARCH_PMD_INDEX(va)) ? 0 : 1;

		pp->guest_temp.pt[which][CO_ARCH_PTE_INDEX(page_va)] =
			co_os_virt_to_phys(p) | _KERNPG_TABLE;
	}

	/* One PD entry per region actually used. */
	pp->guest_temp.pd[0][CO_ARCH_PMD_INDEX(va)] =
		co_os_virt_to_phys(&pp->guest_temp.pt[0]) | _KERNPG_TABLE;

	if (CO_ARCH_PMD_INDEX(last_va) != CO_ARCH_PMD_INDEX(va))
		pp->guest_temp.pd[0][CO_ARCH_PMD_INDEX(last_va)] =
			co_os_virt_to_phys(&pp->guest_temp.pt[1]) | _KERNPG_TABLE;

	pp->guest_temp.pdpt[0][CO_ARCH_PUD_INDEX(va)] = pd_pa   | _KERNPG_TABLE;
	pp->guest_temp.pml4[CO_ARCH_PGD_INDEX(va)]    = pdpt_pa | _KERNPG_TABLE;

	return PTRUE;
}

co_rc_t co_arch_test_switch(co_manager_t* manager, co_arch_switch_test_t* out)
{
	co_arch_passage_page_t* pp;
	co_switch_probe_fn fn;
	unsigned long long va, guest_cr3, host_cr3;
	unsigned long long* sentinel;
	unsigned long code_size;
	int pages = sizeof(co_arch_passage_page_t) / CO_ARCH_PAGE_SIZE;

	co_memset(out, 0, sizeof(*out));
	out->supported = PTRUE;

	code_size = (unsigned long)(&co_switch_probe_code_end - &co_switch_probe_code);
	out->code_size = code_size;
	if (code_size > sizeof(pp->code)) {
		co_debug_error("switch probe is %lu bytes, code area is %lu",
			       code_size, (unsigned long)sizeof(pp->code));
		return CO_RC(ERROR);
	}

	pp = co_os_alloc_exec_pages(pages);
	if (pp == NULL)
		return CO_RC(OUT_OF_MEMORY);

	co_memset(pp, 0, sizeof(*pp));

	va       = (unsigned long long)(size_t)pp;
	host_cr3 = co_get_cr3();

	if (!co_build_guest_tables(pp, va)) {
		co_os_free_exec_pages(pp, pages);
		return CO_RC(ERROR);
	}
	guest_cr3 = co_os_virt_to_phys(&pp->guest_temp.pml4);

	co_memcpy(pp->code, &co_switch_probe_code, code_size);

	/* Both live in first_page, the only page the guest address space maps. */
	fn       = (co_switch_probe_fn)(void*)pp->code;
	sentinel = (unsigned long long*)&pp->params[0];

	out->passage_va = va;
	out->passage_pa = co_os_virt_to_phys(&pp->first_page);
	out->host_cr3   = host_cr3;
	out->guest_cr3  = guest_cr3;
	out->code_va    = (unsigned long long)(size_t)fn;
	out->expected   = CO_SWITCH_SENTINEL;

	fn(host_cr3, guest_cr3, sentinel, CO_SWITCH_SENTINEL);

	out->observed   = *sentinel;
	out->faulted    = (int)pp->params[4];
	out->fault_rip  = pp->params[5];
	out->succeeded  = (out->observed == CO_SWITCH_SENTINEL) ? PTRUE : PFALSE;

	co_os_free_exec_pages(pp, pages);

	return CO_RC(OK);
}

/*
 * The round trip: enter the guest address space, run code there, come back.
 *
 * The guest's state is built to look like the host's except for CR3, the stack
 * and the entry point. That is honest for what this tests -- both sides are the
 * same machine, so the descriptor tables and MSRs are genuinely identical, and
 * restoring them is proven separately.
 */
/* Long-mode interrupt gate: 16 bytes, the offset split across three fields. */
struct co_x86_64_gate {
	unsigned short offset_low;
	unsigned short selector;
	unsigned short flags;
	unsigned short offset_mid;
	unsigned int   offset_high;
	unsigned int   reserved;
} __attribute__((packed));

typedef char co_assert_gate_is_16[(sizeof(struct co_x86_64_gate) == 16) ? 1 : -1];

/*
 * The handler writes its record at literal offsets because it has no register it
 * can trust; the C side reads it back through params[]. Tie the two together so
 * the report cannot silently start reading the wrong words.
 */
typedef char co_assert_fault_slots
	[(__builtin_offsetof(co_arch_passage_page_t, params) + 4 * 8 == CO_PP_FAULTED_N &&
	  __builtin_offsetof(co_arch_passage_page_t, params) + 5 * 8 == CO_PP_FAULT_RIP_N &&
	  __builtin_offsetof(co_arch_passage_page_t, params) + 6 * 8 == CO_PP_COUNTER_N &&
	  __builtin_offsetof(co_arch_passage_page_t, params) + 7 * 8 == CO_PP_REGSUM_N &&
	  __builtin_offsetof(co_arch_passage_page_t, params) + 16 * 8 == CO_PP_VECTOR_N &&
	  __builtin_offsetof(co_arch_passage_page_t, params) + 17 * 8 == CO_PP_ERRCODE_N &&
	  __builtin_offsetof(co_arch_passage_page_t, params) + 18 * 8 == CO_PP_CR2_N &&
	  __builtin_offsetof(co_arch_passage_page_t, params) + 19 * 8 == CO_PP_SWITCH_ENTRY_N &&
	  /* the extern guest reads it as 0x98(%r8), r8 being &params[0] */
	  CO_PP_SWITCH_ENTRY_N - __builtin_offsetof(co_arch_passage_page_t, params) == 0x98 &&
	  __builtin_offsetof(co_arch_passage_page_t, params) + 20 * 8 == CO_PP_CALL_TARGET_N &&
	  __builtin_offsetof(co_arch_passage_page_t, params) + 24 * 8 == CO_PP_CALL_RET_N &&
	  __builtin_offsetof(co_arch_passage_page_t, params) + 28 * 8 == CO_PP_GUEST_FRAME_N &&
	  __builtin_offsetof(co_arch_passage_page_t, params) + 29 * 8 == CO_PP_STEP_N &&
	  __builtin_offsetof(co_arch_passage_page_t, params) + 30 * 8 == CO_PP_BATCH_N &&
	  __builtin_offsetof(co_arch_passage_page_t, params) + 31 * 8 == CO_PP_TRACE_N &&
	  __builtin_offsetof(co_arch_passage_page_t, params) + 47 * 8 == CO_PP_TRACE_IDX_N)
	 ? 1 : -1];

/*
 * The trace ring the stub writes must not run off the end of the page it lives
 * in -- it is addressed from the stub as a literal offset, with no bound beyond
 * this one.
 */
typedef char co_assert_trace_fits
	[(CO_PP_TRACE_IDX_N + 8 <= 0x1000) ? 1 : -1];

/* The blob reaches gdt.base by literal offset; keep it honest. */
typedef char co_assert_gdt_base_offset
	[(__builtin_offsetof(co_arch_state_stack_t, gdt) + 2 == CO_PP_GDT_BASE_N) ? 1 : -1];

/* The stubs need a page to themselves, after the IDT's. */
typedef char co_assert_stubs_fit_host_temp
	[(sizeof(((co_arch_passage_page_t*)0)->host_temp)
	  >= (CO_PP_FPU_PAGE + 1) * 0x1000) ? 1 : -1];

/* The two FXSAVE areas must be 16-byte aligned and inside that page. */
typedef char co_assert_fpu_areas
	[((0xe000 == __builtin_offsetof(co_arch_passage_page_t, host_temp)
		    + CO_PP_FPU_PAGE * 0x1000) &&
	  (0xe000 % 16) == 0 && (0xe200 % 16) == 0) ? 1 : -1];
typedef char co_assert_stub_table_is_a_page
	[((256 * CO_PP_STUB_SIZE) == 0x1000) ? 1 : -1];

/* The IDT lives in host_temp; a full 256-gate table has to fit inside it. */
typedef char co_assert_idt_fits_host_temp
	[(sizeof(((co_arch_passage_page_t*)0)->host_temp) >= 256 * 16) ? 1 : -1];

/*
 * Point every vector at the same handler. Vector identity is not recovered --
 * that needs 256 stubs, one per vector, to push their own number -- but the
 * faulting RIP is, which is enough to know where it went wrong.
 *
 * Note the gates whose vectors push an error code (#PF, #GP, #DF among them)
 * leave the interrupt frame shifted by eight bytes, so the RIP this records is
 * only correct for the vectors that do not. #UD, which the test raises, does not.
 */
/*
 * The 64-bit TSS. In long mode it no longer holds a task context at all -- what
 * survives is RSP0..2 for privilege changes and IST1..7, and IST is the only
 * reason we want one: a gate with a non-zero IST index makes the CPU load RSP
 * from the TSS *before* pushing anything, so a fault is deliverable even when
 * the interrupted stack pointer is garbage.
 */
struct co_x86_64_tss {
	unsigned int	   reserved0;
	unsigned long long rsp[3];
	unsigned long long reserved1;
	unsigned long long ist[7];	/* ist[0] is IST1 */
	unsigned long long reserved2;
	unsigned short	   reserved3;
	unsigned short	   iomap_base;
} __attribute__((packed));

typedef char co_assert_tss_is_104[(sizeof(struct co_x86_64_tss) == 104) ? 1 : -1];

/*
 * A TSS descriptor is sixteen bytes and a system descriptor, so S is 0 and the
 * type is 9 -- "available 64-bit TSS". Type 11 is the same thing already loaded;
 * ltr refuses that, which is why the switch clears the busy bit first.
 */
static void co_build_tss_descriptor(unsigned long long* slot,
				    unsigned long long base,
				    unsigned long limit)
{
	slot[0] = (limit & 0xffffULL)
		| ((base & 0xffffffULL) << 16)
		| (0x9ULL << 40)			/* type 9, S=0 */
		| (0x1ULL << 47)			/* present */
		| (((unsigned long long)(limit >> 16) & 0xfULL) << 48)
		| (((base >> 24) & 0xffULL) << 56);
	slot[1] = (base >> 32) & 0xffffffffULL;
}

static void co_set_gate(struct co_x86_64_gate* gate, unsigned long long handler,
			unsigned short selector)
{
	gate->offset_low  = (unsigned short)(handler & 0xffff);
	gate->selector    = selector;
	/*
	 * present, DPL 0, interrupt gate, IST1. Every vector uses the IST stack,
	 * not just the ones that can arrive on a bad stack -- there is no vector
	 * for which the interrupted RSP is more trustworthy, and a single answer
	 * is one less thing to get wrong.
	 */
	gate->flags       = 0x8e01;
	gate->offset_mid  = (unsigned short)((handler >> 16) & 0xffff);
	gate->offset_high = (unsigned int)(handler >> 32);
	gate->reserved    = 0;
}

/*
 * Does this vector's exception push an error code?
 *
 * #DF(8), #TS(10), #NP(11), #SS(12), #GP(13), #PF(14), #AC(17), #CP(21),
 * #VC(29) and #SX(30). Everything else does not, including every interrupt.
 * Getting this list wrong is not a subtle failure: the handler reads the frame
 * at fixed offsets, so one wrong entry means the reported RIP is whatever
 * happened to be next on the stack.
 */
static bool_t co_vector_has_error_code(int vector)
{
	switch (vector) {
	case 8: case 10: case 11: case 12: case 13:
	case 14: case 17: case 21: case 29: case 30:
		return PTRUE;
	default:
		return PFALSE;
	}
}

/*
 * Emit 256 stubs, one per vector, each pushing its own number and normalising
 * the frame, then jumping to the common handler.
 *
 * Every stub is exactly CO_PP_STUB_SIZE bytes so stub N is simply base + N*16 --
 * no table of addresses to get out of step with the code. Layout:
 *
 *   6a 00                    pushq $0        (or two nops if the CPU pushed one)
 *   68 nn nn nn nn           pushq $vector
 *   e9 rr rr rr rr           jmp   handler
 *   90 ...                   pad to 16
 *
 * push $imm32 rather than the shorter push $imm8, because imm8 is sign-extended
 * and vectors above 127 would arrive as negative numbers.
 *
 * stubs_va is where this page will live *in the guest*, which is the same
 * address the host sees it at -- the passage page is mapped at one address in
 * both. The jmp is relative, so it needs that address to compute its offset.
 */
static void co_build_guest_stubs(unsigned char* stubs, unsigned long long stubs_va,
				 unsigned long long handler_va)
{
	int vector;

	for (vector = 0; vector < 256; vector++) {
		unsigned char* p = stubs + vector * CO_PP_STUB_SIZE;
		unsigned long long stub_va = stubs_va + vector * CO_PP_STUB_SIZE;
		unsigned long long next_va;
		long long rel;
		int i;

		if (co_vector_has_error_code(vector)) {
			p[0] = 0x90;			/* nop  */
			p[1] = 0x90;			/* nop  */
		} else {
			p[0] = 0x6a;			/* pushq $0 -- a stand-in */
			p[1] = 0x00;
		}

		p[2] = 0x68;				/* pushq $imm32 */
		p[3] = (unsigned char)(vector & 0xff);
		p[4] = 0;
		p[5] = 0;
		p[6] = 0;

		/* jmp rel32, measured from the end of the instruction */
		next_va = stub_va + 12;
		rel     = (long long)handler_va - (long long)next_va;

		p[7]  = 0xe9;
		p[8]  = (unsigned char)(rel & 0xff);
		p[9]  = (unsigned char)((rel >> 8) & 0xff);
		p[10] = (unsigned char)((rel >> 16) & 0xff);
		p[11] = (unsigned char)((rel >> 24) & 0xff);

		for (i = 12; i < CO_PP_STUB_SIZE; i++)
			p[i] = 0x90;
	}
}

static void co_build_guest_idt(struct co_x86_64_gate* idt,
			       unsigned long long stubs_va,
			       unsigned short selector)
{
	int vector;

	for (vector = 0; vector < 256; vector++)
		co_set_gate(&idt[vector],
			    stubs_va + vector * CO_PP_STUB_SIZE,
			    selector);
}

/*
 * Build a passage page carrying a guest address space, a GDT and IDT of its own,
 * and a state block derived from the host's, with the guest set to start at
 * entry_offset into the blob.
 *
 * Everything the round-trip and resume tests have in common. They differ only in
 * where the guest starts and what is done with it afterwards, and keeping the
 * setup in one place is what stops those two drifting apart.
 *
 * Returns the page with *out partly filled in, or NULL.
 */
/*
 * reuse: an already-allocated passage page to rebuild in place, or NULL to
 * allocate one.
 *
 * It exists because everything below captures state that belongs to the
 * processor this runs on -- co_arch_save_state() reads GDTR, IDTR, TR and the
 * MSRs of the current core, and the guest state is derived from that copy. A
 * page built on one core and entered from another restores the first core's
 * descriptor tables onto the second, which is not a fault: it is a reset with
 * nothing written, or a core that stops answering the clock.
 *
 * A secondary's page has to be ALLOCATED early, before the guest adopts its
 * tables, so the boot-time graft covers it -- and INITIALISED late, on the
 * thread that will actually enter it. Those are different moments, hence the
 * split.
 */
static co_arch_passage_page_t* co_setup_guest_page_on(co_arch_switch_test_t* out,
						      unsigned long entry_offset,
						      co_arch_passage_page_t* reuse);

static co_arch_passage_page_t* co_setup_guest_page(co_arch_switch_test_t* out,
						   unsigned long entry_offset)
{
	return co_setup_guest_page_on(out, entry_offset, NULL);
}

static co_arch_passage_page_t* co_setup_guest_page_on(co_arch_switch_test_t* out,
						      unsigned long entry_offset,
						      co_arch_passage_page_t* reuse)
{
	co_arch_passage_page_t* pp;
	unsigned long long va, guest_cr3;
	unsigned long blob_size;
	int pages = sizeof(co_arch_passage_page_t) / CO_ARCH_PAGE_SIZE;

	/* The handler reaches these by literal offset; keep them honest. */
	if (__builtin_offsetof(co_arch_passage_page_t, host_state)    != CO_PP_HOST_STATE_N ||
	    __builtin_offsetof(co_arch_passage_page_t, linuxvm_state) != CO_PP_LINUXVM_STATE_N) {
		co_debug_error("passage page layout moved; fault handler offsets are stale");
		return NULL;
	}

	blob_size      = (unsigned long)(&co_switch_full_end - &co_switch_full);
	out->code_size = blob_size;

	if (blob_size > sizeof(pp->code))
		return NULL;

	pp = reuse ? reuse : co_os_alloc_exec_pages(pages);
	if (pp == NULL)
		return NULL;

	co_memset(pp, 0, sizeof(*pp));

	va = (unsigned long long)(size_t)pp;
	if (!co_build_guest_tables(pp, va)) {
		if (!reuse)
			co_os_free_exec_pages(pp, pages);
		return NULL;
	}
	guest_cr3 = co_os_virt_to_phys(&pp->guest_temp.pml4);

	co_memcpy(pp->code, &co_switch_full, blob_size);

	/* Capture the host state, then derive the guest's from it. */
	co_arch_save_state(&pp->host_state);
	co_memcpy(&pp->linuxvm_state, &pp->host_state, sizeof(pp->linuxvm_state));

	/*
	 * A GDT of its own, living in the passage page so it is mapped in the guest
	 * address space. Two entries are enough for a far return: the mandatory null
	 * descriptor, and one 64-bit code segment.
	 *
	 *   0x00209a0000000000 -- P=1 DPL=0 S=1 type=code/read, L=1 (64-bit)
	 *
	 * This is the same thing the real design does: the guest kernel supplies its
	 * own gdt_table and the switch loads it.
	 */
	{
		/*
		 * params[64], not params[8]. Ten entries is eighty bytes, and
		 * from params[8] that runs to params[17] -- straight over
		 * params[16..18], which the fault stub writes the vector, the
		 * error code and CR2 into. The GDT was four entries when it was
		 * put there.
		 */
		unsigned long long* guest_gdt = &pp->params[64];
		unsigned char* host_temp = (unsigned char*)&pp->host_temp;
		struct co_x86_64_tss* tss = (struct co_x86_64_tss*)
			(host_temp + CO_PP_TSS_PAGE * CO_ARCH_PAGE_SIZE);
		unsigned long long ist_top = (unsigned long long)(size_t)
			(host_temp + (CO_PP_ISTSTACK_PAGE + 1) * CO_ARCH_PAGE_SIZE);

		co_memset(tss, 0, sizeof(*tss));
		tss->ist[0]     = ist_top;	/* IST1: a whole page, growing down */
		tss->iomap_base = sizeof(*tss);	/* past the limit: no I/O bitmap */

		/*
		 * Laid out exactly as Linux lays out its own GDT, because the
		 * guest reaches userspace through selectors it does not get to
		 * choose.
		 *
		 * A kernel returning to ring 3 uses __USER_CS and __USER_DS by
		 * number -- iretq loads them from the frame it built, and
		 * sysretq computes them from STAR, which syscall_init() sets to
		 * (__USER32_CS << 48) | (__KERNEL_CS << 32). Those numbers are
		 * fixed in asm/segment.h: index 6 for 64-bit user code, 5 for
		 * user data, 2 for kernel code. A GDT with a null descriptor
		 * and one code segment satisfies a far return between kernel
		 * addresses and nothing else, so the first exec of init raised
		 * #GP with error code 0x30 -- which is not an address, it is
		 * the selector: index 6, __USER_CS, the entry that was not
		 * there.
		 *
		 * So the entries are Linux's, with Linux's values:
		 *
		 *   1 0x08 __KERNEL32_CS   2 0x10 __KERNEL_CS   3 0x18 __KERNEL_DS
		 *   4 0x20 __USER32_CS     5 0x28 __USER_DS     6 0x30 __USER_CS
		 *   8 0x40 the TSS, sixteen bytes, as GDT_ENTRY_TSS
		 *
		 * The guest's own gdt_page is built and maintained by the
		 * kernel as usual; it simply never gets loaded, because lgdt is
		 * still guarded. This table is what GDTR actually points at,
		 * and it now agrees with the kernel about what every selector
		 * means.
		 */
		co_memset(guest_gdt, 0, 10 * 8);
		guest_gdt[0] = 0x0000000000000000ULL;	/* null			*/
		guest_gdt[1] = 0x00cf9b000000ffffULL;	/* 0x08 kernel code, 32	*/
		guest_gdt[2] = 0x00af9b000000ffffULL;	/* 0x10 kernel code, 64	*/
		guest_gdt[3] = 0x00cf93000000ffffULL;	/* 0x18 kernel data	*/
		guest_gdt[4] = 0x00cffb000000ffffULL;	/* 0x20 user code, 32	*/
		guest_gdt[5] = 0x00cff3000000ffffULL;	/* 0x28 user data	*/
		guest_gdt[6] = 0x00affb000000ffffULL;	/* 0x30 user code, 64	*/
		co_build_tss_descriptor(&guest_gdt[8],
					(unsigned long long)(size_t)tss,
					sizeof(*tss) - 1);	/* selector 0x40 */

		pp->linuxvm_state.gdt.base  = (struct x86_dt_entry*)guest_gdt;
		pp->linuxvm_state.gdt.limit = (10 * 8) - 1;
		pp->linuxvm_state.cs        = 0x10;	/* __KERNEL_CS */
		pp->linuxvm_state.tr        = 0x40;	/* GDT_ENTRY_TSS */
		/*
		 * Null, deliberately. Long mode allows it at CPL 0, and it means the
		 * guest never carries a selector that has to be valid in a GDT with
		 * four entries in it.
		 *
		 * ds/es/fs/gs for the same reason, and this became load-bearing the
		 * moment the switch started restoring them. linuxvm_state is copied
		 * from host_state just above, so without this the guest would enter
		 * with Windows' 0x2b/0x53 -- selectors that index past the end of the
		 * four-entry guest GDT. The switch's mov %ax,%ds then #GPs while CR3 is
		 * the guest's, RSP is still the host's (unmapped over there) and TR is
		 * not loaded yet, so the fault cannot be delivered and neither can the
		 * double fault: triple fault, instant reset. Which is exactly what a
		 * host inheriting the guest's zeroed segments used to do in reverse.
		 */
		pp->linuxvm_state.ss        = 0;
		pp->linuxvm_state.ds        = 0;
		pp->linuxvm_state.es        = 0;
		pp->linuxvm_state.fs        = 0;
		pp->linuxvm_state.gs        = 0;
		pp->linuxvm_state.ldt       = 0;

		/*
		 * These are CPU context, not useful defaults for a second OS. In
		 * particular, inheriting an armed Windows hardware breakpoint makes it
		 * fire against an unrelated guest virtual address. Linux will program
		 * its own SYSENTER state during CPU setup; until then, leave it inert.
		 */
		pp->linuxvm_state.cr2         = 0;
		pp->linuxvm_state.dr0         = 0;
		pp->linuxvm_state.dr1         = 0;
		pp->linuxvm_state.dr2         = 0;
		pp->linuxvm_state.dr3         = 0;
		pp->linuxvm_state.dr6         = 0xffff0ff0ULL;
		pp->linuxvm_state.dr7         = 0x400;
		pp->linuxvm_state.sysenter_cs = 0;
		pp->linuxvm_state.sysenter_esp = 0;
		pp->linuxvm_state.sysenter_eip = 0;
		/*
		 * The guest runs at DISPATCH_LEVEL, not at PASSIVE_LEVEL.
		 *
		 * CR8 is IRQL on x64 and it is the processor's task priority:
		 * an interrupt whose vector priority is not above CR8 is not
		 * delivered at all, it stays pending in the local APIC. So this
		 * value decides which of Windows' interrupts the guest is
		 * allowed to receive.
		 *
		 * At 0 the guest received everything, including vector 0x2f --
		 * which is not a device at all. 0x20-0x2f are Windows' software
		 * interrupt levels, and 0x2f is the one the kernel raises to
		 * make a processor run its DPC queue. The guest's stub accepted
		 * it, the host replayed it with `int 0x2f`, and Windows ran its
		 * deferred procedures by hand from inside a driver ioctl, on a
		 * pinned thread, at the wrong IRQL. The captured log shows this
		 * happening 22 switches into a boot, and the bugchecks it
		 * produced land in whatever DPC ran -- netbios and ipnat frames
		 * on the faulting stack, nothing to do with the guest.
		 *
		 * DISPATCH_LEVEL masks exactly 0x20-0x2f while leaving the
		 * clock (0xd1) and IPIs (0xe1) to arrive, so the host still
		 * bounds the guest and its DPCs simply wait -- which is what a
		 * driver holding a processor is supposed to make them do. They
		 * are delivered by Windows itself, at the right IRQL, the
		 * moment the crossing lowers CR8 back to the host's value.
		 */
		pp->linuxvm_state.cr8          = 2;	/* DISPATCH_LEVEL */

		out->guest_gdt = (unsigned long long)(size_t)guest_gdt;
		out->guest_tss = (unsigned long long)(size_t)tss;
		out->ist_stack = ist_top;
	}

	/*
	 * An IDT of its own, also inside the passage page. Without one, IDTR still
	 * points at the host's table after the crossing, and any exception in the
	 * guest is an unreachable handler: double fault, triple fault, reset. With
	 * one, a fault becomes a clean return with a flag set.
	 *
	 * 256 gates of 16 bytes is exactly 4 KB and first_page is full, so it needs a
	 * page of its own. It goes in host_temp, which this path never builds -- and
	 * emphatically NOT at pp + CO_ARCH_PAGE_SIZE, which is guest_temp.pml4: writing
	 * the IDT there overwrites the guest's top-level page table after it has been
	 * built, and the CR3 load then triple faults on the first instruction fetch.
	 * Naming the member rather than doing pointer arithmetic is what keeps that
	 * honest, so leave it named.
	 */
	{
		unsigned char* host_temp = (unsigned char*)&pp->host_temp;
		struct co_x86_64_gate* idt =
			(struct co_x86_64_gate*)(host_temp + CO_PP_IDT_PAGE * CO_ARCH_PAGE_SIZE);
		unsigned char* stubs = host_temp + CO_PP_STUBS_PAGE * CO_ARCH_PAGE_SIZE;
		unsigned long long stubs_va = (unsigned long long)(size_t)stubs;
		unsigned long long handler = (unsigned long long)(size_t)pp->code
			+ (unsigned long)(&co_switch_guest_fault - &co_switch_full);

		/*
		 * Stubs first, then gates pointing at them. 256 stubs of 16 bytes is
		 * exactly a page, which is why they get one of their own.
		 */
		co_build_guest_stubs(stubs, stubs_va, handler);
		/*
		 * 0x10, __KERNEL_CS, and the number matters: since the guest
		 * GDT was laid out as Linux lays out its own, 0x08 is
		 * __KERNEL32_CS -- a 32-bit segment. A gate naming it would
		 * drop the processor out of long mode on the way into the
		 * fault handler, which is a reset with no report, on the exact
		 * path that exists to produce reports.
		 */
		co_build_guest_idt(idt, stubs_va, 0x10);

		pp->linuxvm_state.idt.table = (struct x86_idt_entry*)idt;
		pp->linuxvm_state.idt.size  = (256 * 16) - 1;

		out->guest_idt     = (unsigned long long)(size_t)idt;
		out->guest_stubs   = stubs_va;
		out->fault_handler = handler;
	}

	pp->linuxvm_state.cr3 = guest_cr3;
	/* entry point and stack both inside the passage page, which the guest maps */
	pp->linuxvm_state.return_rip = (unsigned long long)(size_t)pp->code + entry_offset;
	pp->linuxvm_state.rsp        = va + CO_ARCH_PAGE_SIZE - 0x40;

	out->passage_va = va;
	out->passage_pa = co_os_virt_to_phys(&pp->first_page);
	out->host_cr3   = pp->host_state.cr3;
	out->guest_cr3  = guest_cr3;
	out->code_va    = pp->linuxvm_state.return_rip;

	return pp;
}

/*
 * Walk the guest's own tables, in host memory, and confirm one address resolves
 * to the physical page the host has at that same address.
 *
 * The tables are inside the passage page, so they can be read directly rather
 * than through co_os_map(). Returns the level that was absent, or -1 on success.
 */
static int co_preflight_lookup(co_arch_passage_page_t* pp, unsigned long long va)
{
	unsigned long long* table = pp->guest_temp.pml4;
	co_pa_t want = co_os_virt_to_phys((void*)(size_t)va);
	int level;

	for (level = 0; level < 4; level++) {
		unsigned long index;
		unsigned long long entry;

		switch (level) {
		case 0:  index = CO_ARCH_PGD_INDEX(va); break;
		case 1:  index = CO_ARCH_PUD_INDEX(va); break;
		case 2:  index = CO_ARCH_PMD_INDEX(va); break;
		default: index = CO_ARCH_PTE_INDEX(va); break;
		}

		entry = table[index];
		if (!(entry & _PAGE_PRESENT))
			return level;

		if (level == 3) {
			co_pa_t got = (co_pa_t)(entry & CO_ARCH_PAGE_MASK & ~CO_ARCH_PAGE_NX);

			if (got != (want & CO_ARCH_PAGE_MASK)) {
				co_debug_error("preflight: 0x%llx maps to 0x%llx, host has 0x%llx",
					       va, (unsigned long long)got,
					       (unsigned long long)want);
				return 3;
			}
			return -1;
		}

		/*
		 * The next table is a page of this same allocation, so it can be
		 * reached by its host virtual address rather than by mapping the
		 * frame -- which is only true because guest_temp lives inside the
		 * passage page.
		 */
		{
			co_pa_t next_pa = (co_pa_t)(entry & CO_ARCH_PAGE_MASK & ~CO_ARCH_PAGE_NX);
			unsigned long long* candidate = NULL;
			int page;

			for (page = 0; page < (int)(sizeof(*pp) / CO_ARCH_PAGE_SIZE); page++) {
				unsigned char* p = (unsigned char*)pp + page * CO_ARCH_PAGE_SIZE;

				if (co_os_virt_to_phys(p) == next_pa) {
					candidate = (unsigned long long*)p;
					break;
				}
			}

			if (candidate == NULL) {
				co_debug_error("preflight: table at level %d is outside the passage page",
					       level);
				return level;
			}

			table = candidate;
		}
	}

	return -1;
}

/*
 * Refuse to enter a guest address space that cannot run.
 *
 * Every one of the three resets this port has cost was an address the guest
 * needed and did not have: the GDT that lretq reads, the IDT that an exception
 * reads, and a PML4 that had been overwritten. All three are visible by reading
 * the tables, and reading them costs microseconds against a reboot. So nothing
 * loads CR3 until this has passed.
 */
static bool_t co_preflight_guest(co_arch_passage_page_t* pp, unsigned long long va,
				 co_arch_switch_test_t* out)
{
	struct { const char* what; unsigned long long va; } required[] = {
		{ "entry point",  pp->linuxvm_state.return_rip },
		{ "stack",        pp->linuxvm_state.rsp - 8 },
		{ "guest GDT",    (unsigned long long)(size_t)pp->linuxvm_state.gdt.base },
		{ "guest IDT",    (unsigned long long)(size_t)pp->linuxvm_state.idt.table },
		{ "passage page", va },
		{ "guest TSS",    (unsigned long long)(size_t)&pp->host_temp
				  + CO_PP_TSS_PAGE * CO_ARCH_PAGE_SIZE },
		{ "IST stack",    (unsigned long long)(size_t)&pp->host_temp
				  + CO_PP_ISTSTACK_PAGE * CO_ARCH_PAGE_SIZE },
		{ "vector stubs", (unsigned long long)(size_t)&pp->host_temp
				  + CO_PP_STUBS_PAGE * CO_ARCH_PAGE_SIZE },
	};
	const int count = sizeof(required) / sizeof(required[0]);
	int i;

	for (i = 0; i < count; i++) {
		int missing = co_preflight_lookup(pp, required[i].va);

		if (missing >= 0) {
			co_debug_error("preflight: %s at 0x%llx is not usable in the guest "
				       "(absent at level %d) -- refusing to enter",
				       required[i].what, required[i].va, missing);
			out->preflight_failed = PTRUE;
			out->preflight_va     = required[i].va;
			out->preflight_level  = missing;
			return PFALSE;
		}
		out->preflight_checked++;
	}

	return PTRUE;
}

co_rc_t co_arch_test_roundtrip(co_manager_t* manager, co_arch_switch_test_t* out,
			       int provoke_fault)
{
	co_arch_passage_page_t* pp;
	co_switch_full_fn fn;
	unsigned long long* sentinel;
	unsigned long entry_offset;
	int pages = sizeof(co_arch_passage_page_t) / CO_ARCH_PAGE_SIZE;

	co_memset(out, 0, sizeof(*out));
	out->supported = PTRUE;

	entry_offset =
		(provoke_fault == 3)
			? (unsigned long)(&co_switch_guest_entry_badstack - &co_switch_full) :
		(provoke_fault == 2)
			? (unsigned long)(&co_switch_guest_entry_pf - &co_switch_full) :
		(provoke_fault == 1)
			? (unsigned long)(&co_switch_guest_entry_fault - &co_switch_full)
			: (unsigned long)(&co_switch_guest_entry - &co_switch_full);

	pp = co_setup_guest_page(out, entry_offset);
	if (pp == NULL)
		return CO_RC(ERROR);

	fn       = (co_switch_full_fn)(void*)pp->code;
	sentinel = (unsigned long long*)&pp->params[0];
	out->expected = CO_SWITCH_SENTINEL;

	if (!co_preflight_guest(pp, out->passage_va, out)) {
		co_os_free_exec_pages(pp, pages);
		return CO_RC(ERROR);
	}

	fn(&pp->host_state, &pp->linuxvm_state, sentinel, CO_SWITCH_SENTINEL);

	out->observed  = *sentinel;
	/*
	 * Where the fault handler left its record. It cannot be handed a pointer -- it
	 * runs with no trustworthy register -- so it writes at fixed offsets into the
	 * passage page, and params[4] and params[5] are those offsets (0x8e8, 0x8f0).
	 */
	out->faulted    = (int)pp->params[4];
	out->fault_rip  = pp->params[5];
	out->vector     = pp->params[16];
	out->error_code = pp->params[17];
	out->cr2        = pp->params[18];
	out->succeeded  = (out->observed == CO_SWITCH_SENTINEL) ? PTRUE : PFALSE;

	co_os_free_exec_pages(pp, pages);

	return CO_RC(OK);
}

/*
 * Enter the guest repeatedly and require it to pick up where it stopped.
 *
 * This is the monitor loop with the Linux taken out. The first entry lands at the
 * top of the guest loop; every later one resumes *inside* co_switch_full, because
 * the guest's own outbound switch overwrote linuxvm_state.return_rip and .rsp with
 * its resume point. So nothing between iterations may reinitialise those two
 * fields -- doing so would silently turn this back into N independent first
 * entries and the test would pass while proving nothing.
 *
 * Two things are checked, and they fail differently:
 *   counter   -- memory in the passage page. Wrong => the guest did not resume.
 *   reg_accum -- kept in rbx, which is callee-saved and therefore lives on the
 *                guest stack across each crossing. Wrong while counter is right
 *                => the guest resumed but its registers did not survive.
 */
co_rc_t co_arch_test_resume(co_manager_t* manager, co_arch_switch_test_t* out,
			    int iterations)
{
	co_arch_passage_page_t* pp;
	co_switch_full_fn fn;
	unsigned long long* sentinel;
	int pages = sizeof(co_arch_passage_page_t) / CO_ARCH_PAGE_SIZE;
	int i;

	co_memset(out, 0, sizeof(*out));
	out->supported = PTRUE;

	if (iterations < 1 || iterations > 4096)
		iterations = 64;

	pp = co_setup_guest_page(out,
		(unsigned long)(&co_switch_guest_loop - &co_switch_full));
	if (pp == NULL)
		return CO_RC(ERROR);

	fn       = (co_switch_full_fn)(void*)pp->code;
	sentinel = (unsigned long long*)&pp->params[0];

	out->iterations = iterations;
	out->expected   = (unsigned long long)iterations;

	if (!co_preflight_guest(pp, out->passage_va, out)) {
		co_os_free_exec_pages(pp, pages);
		return CO_RC(ERROR);
	}

	for (i = 0; i < iterations; i++)
		fn(&pp->host_state, &pp->linuxvm_state, sentinel, CO_SWITCH_SENTINEL);

	out->counter   = pp->params[6];
	out->reg_accum = pp->params[7];
	out->observed  = out->counter;
	out->faulted   = (int)pp->params[4];
	out->fault_rip = pp->params[5];

	out->succeeded = (out->counter   == (unsigned long long)iterations &&
			  out->reg_accum == (unsigned long long)iterations &&
			  !out->faulted) ? PTRUE : PFALSE;

	co_os_free_exec_pages(pp, pages);

	return CO_RC(OK);
}

/*
 * R4: a guest that lives outside the passage page.
 *
 * Every guest so far has been code sitting inside the passage page, entered in
 * an address space that mapped nothing else. This one is the shape a real guest
 * has: its own text page and its own stack page, at the addresses Linux uses,
 * in a space built a page at a time by co_arch_guest_map -- with the passage
 * page mapped alongside because the switch code, the state blocks, the GDT, the
 * IDT, the stubs and the IST stack all live there and must stay reachable
 * across the crossing.
 *
 * The step being taken is small and specific: instruction fetch after the CR3
 * write now lands somewhere that is not the passage page. Everything else is
 * held constant deliberately, so a failure means the address space is wrong and
 * cannot mean anything else.
 */
#define CO_TEST_GUEST_TEXT	0xffffffff81000000ULL	/* where vmlinux's _text goes */
#define CO_TEST_GUEST_STACK	0xffffffff81004000ULL	/* a gap above it, then a stack */

static co_rc_t co_preflight_space(co_manager_t* manager, co_arch_guest_space_t* space,
				  const char* what, unsigned long long va,
				  co_pa_t expect, co_arch_switch_test_t* out)
{
	co_pa_t got = 0;
	int level = -1;
	co_rc_t rc;

	rc = co_arch_guest_lookup(manager, space, va, &got, &level);

	if (!CO_OK(rc) || (expect != 0 && got != (expect & CO_ARCH_PAGE_MASK))) {
		co_debug_error("preflight: %s at 0x%llx -> 0x%llx (wanted 0x%llx), absent at level %d",
			       what, va, (unsigned long long)got,
			       (unsigned long long)expect, level);
		out->preflight_failed = PTRUE;
		out->preflight_va     = va;
		out->preflight_level  = level;
		return CO_RC(ERROR);
	}

	out->preflight_checked++;
	return CO_RC(OK);
}

co_rc_t co_arch_test_extern_guest(co_manager_t* manager, co_arch_switch_test_t* out,
				  bool_t provoke_fault)
{
	co_arch_passage_page_t* pp;
	co_arch_guest_space_t* space = NULL;
	co_switch_full_fn fn;
	unsigned long long* sentinel;
	co_pfn_t text_pfn = 0, stack_pfn = 0;
	unsigned char* text;
	co_pa_t text_pa, stack_pa;
	unsigned long code_size;
	char* code_start;
	int pages = sizeof(co_arch_passage_page_t) / CO_ARCH_PAGE_SIZE;
	int page;
	co_rc_t rc;

	/*
	 * The result block is a caller's stack local and arrives full of whatever
	 * was there. Clearing it is not hygiene -- preflight_failed is read by the
	 * daemon to decide whether the entry happened at all, so leaving it as
	 * stack litter reports a refusal that never occurred, at an address that
	 * was never checked.
	 */
	co_memset(out, 0, sizeof(*out));
	out->supported = PTRUE;

	/*
	 * Set up the passage page exactly as the round trip does -- same GDT, IDT,
	 * stubs, TSS and state blocks -- then throw away the little hand-rolled
	 * address space it built and construct a real one instead.
	 */
	pp = co_setup_guest_page(out,
		(unsigned long)(&co_switch_guest_entry - &co_switch_full));
	if (pp == NULL)
		return CO_RC(ERROR);

	rc = co_arch_guest_space_create(manager, &space);
	if (!CO_OK(rc))
		goto out_free_pp;

	/* The passage page, at the same address the host has it. */
	for (page = 0; page < pages; page++) {
		unsigned char* p = (unsigned char*)pp + page * CO_ARCH_PAGE_SIZE;

		rc = co_arch_guest_map(manager, space,
				       (unsigned long long)(size_t)p,
				       co_os_virt_to_phys(p), _KERNPG_TABLE);
		if (!CO_OK(rc))
			goto out_free_space;
	}

	/* A text page, holding a guest that knows nothing about where it is. */
	rc = co_os_get_page(manager, &text_pfn);
	if (!CO_OK(rc))
		goto out_free_space;

	code_start = provoke_fault ? &co_extern_guest_fault_code : &co_extern_guest_code;
	code_size  = provoke_fault
		? (unsigned long)(&co_extern_guest_fault_code_end - &co_extern_guest_fault_code)
		: (unsigned long)(&co_extern_guest_code_end - &co_extern_guest_code);

	text = co_os_map(manager, text_pfn);
	if (text == NULL) {
		rc = CO_RC(ERROR);
		goto out_free_text;
	}
	co_memset(text, 0, CO_ARCH_PAGE_SIZE);
	co_memcpy(text, code_start, code_size);
	co_os_unmap(manager, text, text_pfn);

	text_pa = ((co_pa_t)text_pfn) << CO_ARCH_PAGE_SHIFT;
	rc = co_arch_guest_map(manager, space, CO_TEST_GUEST_TEXT, text_pa, _KERNPG_TABLE);
	if (!CO_OK(rc))
		goto out_free_text;

	/* And a stack page of its own, rather than growing down into the state blocks. */
	rc = co_os_get_page(manager, &stack_pfn);
	if (!CO_OK(rc))
		goto out_free_text;

	stack_pa = ((co_pa_t)stack_pfn) << CO_ARCH_PAGE_SHIFT;
	rc = co_arch_guest_map(manager, space, CO_TEST_GUEST_STACK, stack_pa, _KERNPG_TABLE);
	if (!CO_OK(rc))
		goto out_free_stack;

	/*
	 * Point the guest at its new home. Note return_rip and rsp are now
	 * addresses in the guest's own space with no host meaning at all -- the
	 * first time that has been true.
	 */
	pp->linuxvm_state.cr3        = co_arch_guest_space_root(space);
	pp->linuxvm_state.return_rip = CO_TEST_GUEST_TEXT;
	pp->linuxvm_state.rsp        = CO_TEST_GUEST_STACK + CO_ARCH_PAGE_SIZE - 0x40;

	/* How the guest reaches the switch: it cannot jump there relatively. */
	pp->params[19] = (unsigned long long)(size_t)pp->code;

	fn       = (co_switch_full_fn)(void*)pp->code;
	sentinel = (unsigned long long*)&pp->params[0];

	out->guest_cr3   = pp->linuxvm_state.cr3;
	out->code_va     = CO_TEST_GUEST_TEXT;
	out->guest_text  = CO_TEST_GUEST_TEXT;
	out->guest_stack = CO_TEST_GUEST_STACK;
	out->tables      = co_arch_guest_space_tables(space);
	out->expected    = CO_SWITCH_SENTINEL;

	/*
	 * Walk the real tables before trusting them. The passage page is checked
	 * at its first and last page, since the whole run depends on every one of
	 * them being present and a partial mapping is the likely mistake.
	 */
	if (!CO_OK(co_preflight_space(manager, space, "guest text", CO_TEST_GUEST_TEXT,
				      text_pa, out)) ||
	    !CO_OK(co_preflight_space(manager, space, "guest stack", CO_TEST_GUEST_STACK,
				      stack_pa, out)) ||
	    !CO_OK(co_preflight_space(manager, space, "passage page, first",
				      (unsigned long long)(size_t)pp,
				      co_os_virt_to_phys(pp), out)) ||
	    !CO_OK(co_preflight_space(manager, space, "passage page, last",
				      (unsigned long long)(size_t)pp + (pages - 1) * CO_ARCH_PAGE_SIZE,
				      co_os_virt_to_phys((unsigned char*)pp
							 + (pages - 1) * CO_ARCH_PAGE_SIZE),
				      out)) ||
	    !CO_OK(co_preflight_space(manager, space, "guest IDT",
				      (unsigned long long)(size_t)pp->linuxvm_state.idt.table,
				      co_os_virt_to_phys(pp->linuxvm_state.idt.table), out))) {
		rc = CO_RC(ERROR);
		goto out_free_stack;
	}

	fn(&pp->host_state, &pp->linuxvm_state, sentinel, CO_SWITCH_SENTINEL);

	out->observed   = *sentinel;
	out->faulted    = (int)pp->params[4];
	out->fault_rip  = pp->params[5];
	out->vector     = pp->params[16];
	out->error_code = pp->params[17];
	out->cr2        = pp->params[18];

	out->succeeded = provoke_fault
		? ((out->faulted && out->vector == 6) ? PTRUE : PFALSE)
		: ((out->observed == CO_SWITCH_SENTINEL && !out->faulted) ? PTRUE : PFALSE);

	rc = CO_RC(OK);

out_free_stack:
	if (stack_pfn)
		co_os_put_page(manager, stack_pfn);
out_free_text:
	if (text_pfn)
		co_os_put_page(manager, text_pfn);
out_free_space:
	co_arch_guest_space_destroy(manager, space);
out_free_pp:
	co_os_free_exec_pages(pp, pages);

	return rc;
}

/*
 * Enter a guest address space that already holds a loaded kernel image.
 *
 * The image is mapped at its link addresses by co_kload_*; what is missing is
 * everything the crossing itself needs -- the passage page with the switch code,
 * the state blocks, the GDT, the IDT, the stubs, the TSS and the IST stack. Those
 * are mapped in alongside, at the addresses the host has them, which is what
 * keeps other_map zero.
 *
 * entry_va is somewhere inside the loaded image, and the few bytes there are
 * replaced with the same stub R4 used. That is a stand-in for a cooperative
 * entry point compiled into the kernel: the real thing will be a symbol the
 * image already contains, but the mechanism being tested -- transfer control to
 * an address inside a mapped kernel image, and get back -- is the same either
 * way, and it can be tested before the kernel is patched at all.
 */
co_rc_t co_arch_enter_loaded(co_manager_t* manager, co_arch_guest_space_t* space,
			     unsigned long long entry_va, co_arch_switch_test_t* out)
{
	co_arch_passage_page_t* pp;
	co_switch_full_fn fn;
	unsigned long long* sentinel;
	unsigned long long stack_va = 0xffff908000000000ULL;
	co_pfn_t stack_pfn = 0;
	co_pa_t entry_pa = 0;
	unsigned char* p;
	unsigned long code_size;
	int pages = sizeof(co_arch_passage_page_t) / CO_ARCH_PAGE_SIZE;
	int page, level = -1;
	co_rc_t rc;

	co_memset(out, 0, sizeof(*out));
	out->supported = PTRUE;

	if (space == NULL)
		return CO_RC(ERROR);

	pp = co_setup_guest_page(out,
		(unsigned long)(&co_switch_guest_entry - &co_switch_full));
	if (pp == NULL)
		return CO_RC(ERROR);

	for (page = 0; page < pages; page++) {
		unsigned char* q = (unsigned char*)pp + page * CO_ARCH_PAGE_SIZE;

		rc = co_arch_guest_map(manager, space, (unsigned long long)(size_t)q,
				       co_os_virt_to_phys(q), _KERNPG_TABLE);
		if (!CO_OK(rc))
			goto out_free_pp;
	}

	/* A stack above the image, rather than anywhere the kernel believes it owns. */
	rc = co_os_get_page(manager, &stack_pfn);
	if (!CO_OK(rc))
		goto out_free_pp;

	rc = co_arch_guest_map(manager, space, stack_va,
			       ((co_pa_t)stack_pfn) << CO_ARCH_PAGE_SHIFT, _KERNPG_TABLE);
	if (!CO_OK(rc))
		goto out_free_stack;

	/* Write the stub into the image, through the guest's own tables. */
	rc = co_arch_guest_lookup(manager, space, entry_va, &entry_pa, &level);
	if (!CO_OK(rc) || !entry_pa) {
		co_debug_error("enter: 0x%llx is not mapped in the loaded image (level %d)",
			       entry_va, level);
		rc = CO_RC(ERROR);
		goto out_free_stack;
	}

	code_size = (unsigned long)(&co_extern_guest_code_end - &co_extern_guest_code);

	p = co_os_map(manager, (co_pfn_t)(entry_pa >> CO_ARCH_PAGE_SHIFT));
	if (p == NULL) {
		rc = CO_RC(ERROR);
		goto out_free_stack;
	}
	co_memcpy(p + (entry_va & ~CO_ARCH_PAGE_MASK), &co_extern_guest_code, code_size);
	co_os_unmap(manager, p, (co_pfn_t)(entry_pa >> CO_ARCH_PAGE_SHIFT));

	pp->linuxvm_state.cr3        = co_arch_guest_space_root(space);
	pp->linuxvm_state.return_rip = entry_va;
	pp->linuxvm_state.rsp        = stack_va + CO_ARCH_PAGE_SIZE - 0x40;
	pp->params[19]               = (unsigned long long)(size_t)pp->code;

	fn       = (co_switch_full_fn)(void*)pp->code;
	sentinel = (unsigned long long*)&pp->params[0];

	out->guest_cr3   = pp->linuxvm_state.cr3;
	out->code_va     = entry_va;
	out->guest_text  = entry_va;
	out->guest_stack = stack_va;
	out->tables      = co_arch_guest_space_tables(space);
	out->expected    = CO_SWITCH_SENTINEL;

	if (!CO_OK(co_preflight_space(manager, space, "entry point", entry_va, entry_pa, out)) ||
	    !CO_OK(co_preflight_space(manager, space, "stack", stack_va,
				      ((co_pa_t)stack_pfn) << CO_ARCH_PAGE_SHIFT, out)) ||
	    !CO_OK(co_preflight_space(manager, space, "passage page",
				      (unsigned long long)(size_t)pp,
				      co_os_virt_to_phys(pp), out)) ||
	    !CO_OK(co_preflight_space(manager, space, "guest IDT",
				      (unsigned long long)(size_t)pp->linuxvm_state.idt.table,
				      co_os_virt_to_phys(pp->linuxvm_state.idt.table), out))) {
		rc = CO_RC(ERROR);
		goto out_free_stack;
	}

	fn(&pp->host_state, &pp->linuxvm_state, sentinel, CO_SWITCH_SENTINEL);

	out->observed   = *sentinel;
	out->faulted    = (int)pp->params[4];
	out->fault_rip  = pp->params[5];
	out->vector     = pp->params[16];
	out->error_code = pp->params[17];
	out->cr2        = pp->params[18];
	out->succeeded  = (out->observed == CO_SWITCH_SENTINEL && !out->faulted) ? PTRUE : PFALSE;

	rc = CO_RC(OK);

out_free_stack:
	if (stack_pfn)
		co_os_put_page(manager, stack_pfn);
out_free_pp:
	co_os_free_exec_pages(pp, pages);

	return rc;
}

/*
 * Run functions the loaded kernel compiled.
 *
 * Everything up to R5 executed instructions this project wrote. This executes
 * Linux's own: memset and strlen, called with a SysV frame, returning through
 * the unpatched return thunk like any other caller.
 *
 * Both are chosen because they are leaves -- they touch their arguments and
 * nothing else, no per-cpu data, no other kernel state -- and because each is
 * checkable in a different way. memset is confirmed by reading the bytes it
 * claims to have written; strlen by the number it returns. A stub that merely
 * returned plausibly would fail one or the other.
 *
 * Note memset here is the unpatched alternative, which jumps straight to
 * memset_orig, the generic byte loop. apply_alternatives() has never run, so the
 * default instruction stream is what executes -- still code the kernel's own
 * assembler emitted, just not the ERMS variant a booted kernel would select.
 */
/*
 * The ring the patched kernel's early console writes into. Its layout is fixed
 * by the kernel side (patch/7.1.5/early-console.diff) and must match exactly:
 * two 64-bit counters followed by the text.
 */
struct co_console_ring {
	unsigned long long written;
	unsigned long long capacity;
	char		   text[];
};

/* Write one 64-bit value into the guest, through the guest's own tables. */
static co_rc_t co_write_guest_u64(co_manager_t* manager, co_arch_guest_space_t* space,
				  unsigned long long va, unsigned long long value)
{
	co_pa_t pa = 0;
	int level = -1;
	unsigned char* p;

	if (!CO_OK(co_arch_guest_lookup(manager, space, va, &pa, &level)) || !pa) {
		co_debug_error("poke: 0x%llx is not mapped (level %d)", va, level);
		return CO_RC(NOT_FOUND);
	}

	p = co_os_map(manager, (co_pfn_t)(pa >> CO_ARCH_PAGE_SHIFT));
	if (p == NULL)
		return CO_RC(ERROR);

	*(unsigned long long*)(p + (va & ~CO_ARCH_PAGE_MASK)) = value;
	co_os_unmap(manager, p, (co_pfn_t)(pa >> CO_ARCH_PAGE_SHIFT));

	return CO_RC(OK);
}

#define CO_TEST_SCRATCH		0xffffffff91000000ULL
#define CO_TEST_PATTERN		0x5a
#define CO_TEST_STRING		"hello from a cooperative guest"

static co_rc_t co_call_loaded_once(co_manager_t* manager, co_arch_passage_page_t* pp,
				   co_switch_full_fn fn, unsigned long long stack_va,
				   unsigned long long target,
				   unsigned long long a0, unsigned long long a1,
				   unsigned long long a2, unsigned long long a3,
				   unsigned long long a4, unsigned long long* ret)
{
	pp->params[20] = target;
	pp->params[21] = a0;
	pp->params[22] = a1;
	pp->params[23] = a2;
	pp->params[24] = 0;
	pp->params[25] = a3;
	pp->params[26] = a4;
	pp->params[27] = 0;

	/*
	 * Both have to be reset before every call. The outbound switch overwrites
	 * return_rip and rsp with wherever the guest was when it left, so a second
	 * call that did not restore them would resume the previous one instead of
	 * starting a new one.
	 */
	pp->linuxvm_state.return_rip = (unsigned long long)(size_t)pp->code
		+ (unsigned long)(&co_call_shim - &co_switch_full);
	pp->linuxvm_state.rsp = stack_va + CO_ARCH_PAGE_SIZE - 0x40;

	pp->params[4] = 0;	/* faulted */

	fn(&pp->host_state, &pp->linuxvm_state, NULL, 0);

	if (pp->params[4]) {
		co_debug_error("call to 0x%llx faulted: vector %llu at 0x%llx",
			       target, pp->params[16], pp->params[5]);
		return CO_RC(ERROR);
	}

	*ret = pp->params[24];

	return CO_RC(OK);
}

co_rc_t co_arch_test_kernel_code(co_manager_t* manager, co_arch_guest_space_t* space,
				 unsigned long long memset_va,
				 unsigned long long strlen_va,
				 unsigned long long snprintf_va,
				 unsigned long long early_printk_va,
				 unsigned long long early_console_va,
				 unsigned long long colinux_console_va,
				 unsigned long long ring_symbol_va,
				 co_arch_kcall_test_t* out)
{
	co_arch_passage_page_t* pp;
	co_arch_switch_test_t setup = {0, };
	co_switch_full_fn fn;
	unsigned long long stack_va = 0xffff908000000000ULL;
	co_pfn_t stack_pfn = 0, scratch_pfn = 0;
	unsigned char* p;
	int pages = sizeof(co_arch_passage_page_t) / CO_ARCH_PAGE_SIZE;
	int page, i;
	co_rc_t rc;

	co_memset(out, 0, sizeof(*out));
	out->supported  = PTRUE;
	out->scratch_va = CO_TEST_SCRATCH;

	if (space == NULL || !memset_va || !strlen_va)
		return CO_RC(ERROR);

	pp = co_setup_guest_page(&setup,
		(unsigned long)(&co_switch_guest_entry - &co_switch_full));
	if (pp == NULL)
		return CO_RC(ERROR);

	for (page = 0; page < pages; page++) {
		unsigned char* q = (unsigned char*)pp + page * CO_ARCH_PAGE_SIZE;

		rc = co_arch_guest_map(manager, space, (unsigned long long)(size_t)q,
				       co_os_virt_to_phys(q), _KERNPG_TABLE);
		if (!CO_OK(rc))
			goto out_free_pp;
	}

	rc = co_os_get_page(manager, &stack_pfn);
	if (!CO_OK(rc))
		goto out_free_pp;
	rc = co_arch_guest_map(manager, space, stack_va,
			       ((co_pa_t)stack_pfn) << CO_ARCH_PAGE_SHIFT, _KERNPG_TABLE);
	if (!CO_OK(rc))
		goto out_free_stack;

	rc = co_os_get_page(manager, &scratch_pfn);
	if (!CO_OK(rc))
		goto out_free_stack;
	rc = co_arch_guest_map(manager, space, CO_TEST_SCRATCH,
			       ((co_pa_t)scratch_pfn) << CO_ARCH_PAGE_SHIFT, _KERNPG_TABLE);
	if (!CO_OK(rc))
		goto out_free_scratch;

	/* Fill the scratch page with something memset must overwrite. */
	p = co_os_map(manager, scratch_pfn);
	if (p == NULL) {
		rc = CO_RC(ERROR);
		goto out_free_scratch;
	}
	co_memset(p, 0xa5, CO_ARCH_PAGE_SIZE);
	co_os_unmap(manager, p, scratch_pfn);

	pp->linuxvm_state.cr3 = co_arch_guest_space_root(space);
	pp->params[19]        = (unsigned long long)(size_t)pp->code;
	fn                    = (co_switch_full_fn)(void*)pp->code;

	/* --- memset(scratch, 0x5a, 4096) --- */
	out->memset_va = memset_va;
	rc = co_call_loaded_once(manager, pp, fn, stack_va, memset_va,
				 CO_TEST_SCRATCH, CO_TEST_PATTERN, CO_ARCH_PAGE_SIZE,
				 0, 0, &out->memset_ret);
	if (!CO_OK(rc)) {
		out->faulted   = (int)pp->params[4];
		out->vector    = pp->params[16];
		out->fault_rip = pp->params[5];
		out->cr2       = pp->params[18];
		goto out_free_scratch;
	}

	/* Did it actually write? Read the page the guest wrote, not its word for it. */
	p = co_os_map(manager, scratch_pfn);
	if (p == NULL) {
		rc = CO_RC(ERROR);
		goto out_free_scratch;
	}
	out->pattern_ok = PTRUE;
	for (i = 0; i < (int)CO_ARCH_PAGE_SIZE; i++) {
		if (p[i] != CO_TEST_PATTERN) {
			out->pattern_ok  = PFALSE;
			out->first_bad   = i;
			out->first_bad_byte = p[i];
			break;
		}
	}

	/* Now a string for strlen, placed after memset so it cannot be a leftover. */
	co_memcpy(p, CO_TEST_STRING, sizeof(CO_TEST_STRING));
	co_os_unmap(manager, p, scratch_pfn);

	/* --- strlen(scratch) --- */
	out->strlen_va = strlen_va;
	rc = co_call_loaded_once(manager, pp, fn, stack_va, strlen_va,
				 CO_TEST_SCRATCH, 0, 0, 0, 0, &out->strlen_ret);
	if (!CO_OK(rc)) {
		out->faulted   = (int)pp->params[4];
		out->vector    = pp->params[16];
		out->fault_rip = pp->params[5];
		out->cr2       = pp->params[18];
		goto out_free_scratch;
	}

	out->strlen_expected = sizeof(CO_TEST_STRING) - 1;

	/*
	 * --- snprintf, which is the point of the exercise ---
	 *
	 * memset and strlen are byte loops. vsnprintf is the kernel's whole
	 * formatting engine: parsing, width and precision, integer conversion,
	 * lookup tables. If it runs, most of the kernel's non-stateful code runs,
	 * and printk is built directly on it -- so testing it on its own means
	 * that when output later comes out wrong we already know the formatter
	 * is not the reason.
	 *
	 * No %p. That path hashes pointers with a key a workqueue initialises,
	 * and nothing here has initialised anything.
	 */
	if (snprintf_va) {
		static const char fmt[] = "colinux: %s, %d-bit, ok";
		static const char arg[] = "x86-64";
		static const char want[] = "colinux: x86-64, 64-bit, ok";
		unsigned long long fmt_va = CO_TEST_SCRATCH + 0x800;
		unsigned long long arg_va = CO_TEST_SCRATCH + 0x900;
		int n;

		p = co_os_map(manager, scratch_pfn);
		if (p == NULL) {
			rc = CO_RC(ERROR);
			goto out_free_scratch;
		}
		co_memset(p, 0, 0x400);
		co_memcpy(p + 0x800, fmt, sizeof(fmt));
		co_memcpy(p + 0x900, arg, sizeof(arg));
		co_os_unmap(manager, p, scratch_pfn);

		out->snprintf_va = snprintf_va;
		rc = co_call_loaded_once(manager, pp, fn, stack_va, snprintf_va,
					 CO_TEST_SCRATCH, 256, fmt_va,
					 arg_va, 64, &out->snprintf_ret);
		if (!CO_OK(rc)) {
			out->faulted   = (int)pp->params[4];
			out->vector    = pp->params[16];
			out->fault_rip = pp->params[5];
			out->cr2       = pp->params[18];
			goto out_free_scratch;
		}

		/* Read what the kernel formatted, out of the guest's own memory. */
		p = co_os_map(manager, scratch_pfn);
		if (p == NULL) {
			rc = CO_RC(ERROR);
			goto out_free_scratch;
		}
		for (n = 0; n < (int)sizeof(out->text) - 1 && p[n]; n++)
			out->text[n] = p[n];
		out->text[n] = 0;
		co_os_unmap(manager, p, scratch_pfn);

		out->snprintf_expected = sizeof(want) - 1;
		out->text_ok = PTRUE;
		for (n = 0; n < (int)sizeof(want) - 1; n++) {
			if (out->text[n] != want[n]) {
				out->text_ok = PFALSE;
				break;
			}
		}
	}

	/*
	 * --- the patched kernel's own early console ---
	 *
	 * Two globals are poked before anything runs: co_colinux_console_ring, so
	 * the console knows where to write, and early_console, so early_printk has
	 * a console at all. The second is the trick a real machine cannot use --
	 * it would have to reach setup_early_printk() during setup_arch() first,
	 * and everything before that point is silent.
	 *
	 * Then early_printk() is called exactly as kernel code calls it.
	 */
	if (early_printk_va && early_console_va && colinux_console_va && ring_symbol_va) {
		unsigned char* host_temp = (unsigned char*)&pp->host_temp;
		struct co_console_ring* ring = (struct co_console_ring*)
			(host_temp + CO_PP_CONSOLE_PAGE * CO_ARCH_PAGE_SIZE);
		unsigned long long ring_va = (unsigned long long)(size_t)ring;
		static const char cfmt[] = "colinux: early console alive, %s, %d-bit\n";
		static const char carg[] = "x86-64";
		unsigned long long fmt_va = CO_TEST_SCRATCH + 0xa00;
		unsigned long long arg_va = CO_TEST_SCRATCH + 0xb00;
		unsigned long long ignored = 0;
		int n;

		co_memset(ring, 0, CO_ARCH_PAGE_SIZE);
		ring->capacity = CO_ARCH_PAGE_SIZE - sizeof(*ring);

		p = co_os_map(manager, scratch_pfn);
		if (p == NULL) {
			rc = CO_RC(ERROR);
			goto out_free_scratch;
		}
		co_memcpy(p + 0xa00, cfmt, sizeof(cfmt));
		co_memcpy(p + 0xb00, carg, sizeof(carg));
		co_os_unmap(manager, p, scratch_pfn);

		if (!CO_OK(co_write_guest_u64(manager, space, ring_symbol_va, ring_va)) ||
		    !CO_OK(co_write_guest_u64(manager, space, early_console_va,
					      colinux_console_va))) {
			co_debug_error("could not poke the console globals");
			rc = CO_RC(ERROR);
			goto out_free_scratch;
		}

		out->console_ring_va = ring_va;

		rc = co_call_loaded_once(manager, pp, fn, stack_va, early_printk_va,
					 fmt_va, arg_va, 64, 0, 0, &ignored);
		if (!CO_OK(rc)) {
			out->faulted   = (int)pp->params[4];
			out->vector    = pp->params[16];
			out->fault_rip = pp->params[5];
			out->cr2       = pp->params[18];
			goto out_free_scratch;
		}

		out->console_written  = ring->written;
		out->console_capacity = ring->capacity;
		for (n = 0; n < (int)sizeof(out->console_text) - 1 && n < (int)ring->written; n++)
			out->console_text[n] = ring->text[n];
		out->console_text[n] = 0;
		out->console_ok = (ring->written > 0) ? PTRUE : PFALSE;
	}

	out->succeeded = (out->pattern_ok &&
			  out->memset_ret == CO_TEST_SCRATCH &&
			  out->strlen_ret == out->strlen_expected &&
			  (!snprintf_va || (out->text_ok &&
					    out->snprintf_ret == out->snprintf_expected)))
			 ? PTRUE : PFALSE;

	rc = CO_RC(OK);

out_free_scratch:
	if (scratch_pfn)
		co_os_put_page(manager, scratch_pfn);
out_free_stack:
	if (stack_pfn)
		co_os_put_page(manager, stack_pfn);
out_free_pp:
	co_os_free_exec_pages(pp, pages);

	return rc;
}

/*
 * Boot the loaded kernel: enter co_arch_start_kernel and see how far it gets.
 *
 * Three globals are written before anything runs, all of them things the host
 * can do and a real machine cannot:
 *
 *   co_colinux_console_ring  where the early console writes
 *   early_console            so early_printk has a console from instruction one
 *   initial_code             start_kernel rather than x86_64_start_kernel
 *
 * The last is the same substitution the i386 port makes. x86_64_start_kernel
 * calls reset_early_page_tables() and assigns init_top_pgt[511], which would
 * discard the address space we are running in, and idt_setup_early_handler(),
 * which would replace the fault stubs during the window they are most needed.
 * What it does that we want -- clearing .bss -- is already true, because every
 * page kload allocates is zeroed and .bss arrives as zeroing chunks.
 *
 * This is expected to stop somewhere. The point is that it can now say where:
 * whatever it printed before dying is in the ring, and if it faults the stubs
 * report the vector, RIP and CR2.
 */
/*
 * Hand an interrupt that arrived in the guest to the host that owns it.
 *
 * The interrupt really was delivered by hardware -- it vectored through the
 * guest's IDT into our stub -- so the APIC's in-service bit is set and Windows'
 * own handler has to run, both to do the work and to send EOI. Swallowing it
 * instead leaves that priority level masked and everything at or below it stops
 * being delivered, which wedges the machine as thoroughly as a hang.
 *
 * The frame is synthesised the way hardware would build it. iretq pops five
 * quadwords whether or not the privilege level changed, so SS and RSP have to
 * be there and not just flags, CS and RIP.
 */
/*
 * A snapshot of everything about the host CPU that a crossing could damage.
 *
 * Deliberately wider than what the switch saves and restores. The registers it
 * already handles are checked to prove it handled them; the ones it does not
 * are checked because "the guest cannot have touched that" is exactly the kind
 * of assumption that has been wrong three times today -- CR4 was carried across
 * unchanged, the guest's IDT load was believed harmless, and the GDT was
 * believed to travel safely with the address space.
 */
typedef struct {
	unsigned long	   cpu;
	unsigned long long cr0, cr2, cr4, cr3;
	unsigned long long gdt_base, idt_base;
	unsigned long long fs_base, gs_base, kernel_gs_base;
	unsigned long long lstar, star, cstar, sfmask, efer;
	unsigned long long sysenter_cs, sysenter_esp, sysenter_eip;
	unsigned long long dr0, dr1, dr2, dr3, dr6;
	unsigned long long cr8, pat, dr7, rflags, xcr0;
	unsigned long long mtrr_def;
	unsigned short	   gdt_limit, idt_limit, ldt, tr, cs, ss;
	unsigned short	   ds, es, fs, gs;
} co_host_snapshot_t;

static unsigned long long co_rdmsr(unsigned int msr)
{
	unsigned int lo, hi;

	asm volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));

	return ((unsigned long long)hi << 32) | lo;
}

static void co_wrmsr(unsigned int msr, unsigned long long value)
{
	asm volatile("wrmsr" : : "c"(msr), "a"((unsigned int)value),
		     "d"((unsigned int)(value >> 32)));
}

/*
 * Put back the pieces of host state that can be put back.
 *
 * The self-check's job is to name what moved; this is the companion that keeps
 * the box alive to hear the answer. PAT, CR8 and DR7 are plain values with no
 * side effects on write, so restoring the snapshot is always safe -- and a
 * no-op when nothing moved. The descriptor tables and MSRs the switch itself
 * carries are deliberately not touched here: if those come back wrong the
 * crossing machinery is broken and no repair from this side is trustworthy.
 */
static void co_host_repair(const co_host_snapshot_t* want,
			   co_host_field_t bad)
{
	unsigned int lo, hi;

	co_wrmsr(0x277, want->pat);
	co_wrmsr(0x2ff, want->mtrr_def);
	asm volatile("mov %0, %%cr8" : : "r"(want->cr8));
	asm volatile("mov %0, %%cr2" : : "r"(want->cr2));
	if (want->cr4 & (1ULL << 18)) {
		lo = (unsigned int)want->xcr0;
		hi = (unsigned int)(want->xcr0 >> 32);
		asm volatile("xsetbv" : : "a"(lo), "d"(hi), "c"(0));
	}

	co_wrmsr(0xc0000083, want->cstar);
	co_wrmsr(0x174, want->sysenter_cs);
	co_wrmsr(0x175, want->sysenter_esp);
	co_wrmsr(0x176, want->sysenter_eip);

	/* Disarm first, restore the address/status registers, then arm last. */
	asm volatile("mov %0, %%dr7" : : "r"(0x400ULL));
	asm volatile("mov %0, %%dr0" : : "r"(want->dr0));
	asm volatile("mov %0, %%dr1" : : "r"(want->dr1));
	asm volatile("mov %0, %%dr2" : : "r"(want->dr2));
	asm volatile("mov %0, %%dr3" : : "r"(want->dr3));
	asm volatile("mov %0, %%dr6" : : "r"(want->dr6));
	asm volatile("mov %0, %%dr7" : : "r"(want->dr7));

	/* LLDT depends on the GDT, so only do it when that is the named mismatch. */
	if (bad == CO_HOST_FIELD_LDT) {
		unsigned short ldt = want->ldt;
		asm volatile("lldt %0" : : "m"(ldt));
	}
}

static void co_host_snapshot(co_host_snapshot_t* s)
{
	struct { unsigned short limit; unsigned long long base; } __attribute__((packed)) dt;
	unsigned long long v;
	unsigned int lo, hi;
	unsigned short w;

	s->cpu = co_os_current_cpu();

	asm volatile("mov %%cr0, %0" : "=r"(v)); s->cr0 = v;
	asm volatile("mov %%cr2, %0" : "=r"(v)); s->cr2 = v;
	asm volatile("mov %%cr4, %0" : "=r"(v)); s->cr4 = v;
	s->xcr0 = 0;
	if (s->cr4 & (1ULL << 18)) {
		asm volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
		s->xcr0 = ((unsigned long long)hi << 32) | lo;
	}
	asm volatile("mov %%cr3, %0" : "=r"(v)); s->cr3 = v;
	asm volatile("pushfq; popq %0" : "=r"(v) : : "memory");
	/* Ignore arithmetic flags, which ordinary C code changes constantly. */
	s->rflags = v & 0x00000000001e7700ULL; /* TF IF DF IOPL NT VM AC VIF VIP */

	asm volatile("sgdt %0" : "=m"(dt));
	s->gdt_base = dt.base; s->gdt_limit = dt.limit;
	asm volatile("sidt %0" : "=m"(dt));
	s->idt_base = dt.base; s->idt_limit = dt.limit;

	asm volatile("sldt %0"   : "=r"(w)); s->ldt = w;
	asm volatile("str %0"    : "=r"(w)); s->tr = w;
	asm volatile("mov %%cs, %0" : "=r"(w)); s->cs = w;
	asm volatile("mov %%ss, %0" : "=r"(w)); s->ss = w;
	/*
	 * The data segment selectors, now that the switch restores them. They
	 * are not covered above by cs/ss, and a wrong one is exactly the
	 * delayed freeze this check exists to catch: Windows runs on a bad ds
	 * or gs for a while and dies somewhere unrelated.
	 */
	asm volatile("mov %%ds, %0" : "=r"(w)); s->ds = w;
	asm volatile("mov %%es, %0" : "=r"(w)); s->es = w;
	asm volatile("mov %%fs, %0" : "=r"(w)); s->fs = w;
	asm volatile("mov %%gs, %0" : "=r"(w)); s->gs = w;

	s->fs_base	  = co_rdmsr(0xc0000100);
	s->gs_base	  = co_rdmsr(0xc0000101);
	s->kernel_gs_base = co_rdmsr(0xc0000102);
	s->star		  = co_rdmsr(0xc0000081);
	s->lstar	  = co_rdmsr(0xc0000082);
	s->cstar	  = co_rdmsr(0xc0000083);
	s->sfmask	  = co_rdmsr(0xc0000084);
	s->efer		  = co_rdmsr(0xc0000080);
	s->sysenter_cs  = co_rdmsr(0x174);
	s->sysenter_esp = co_rdmsr(0x175);
	s->sysenter_eip = co_rdmsr(0x176);

	/*
	 * State a free-running boot can plausibly write that nothing above
	 * covers, added after the first cooperative run completed cleanly --
	 * every check above passing -- and the box froze anyway, minutes later.
	 *
	 * CR8 is IRQL on x64; the switch now carries it both directions, and
	 * this snapshot is what proves that keeps being true. PAT is the memory
	 * type table Linux's pat_init rewrites to its own layout, changing what
	 * Windows' live PTEs mean out from under it. DR7 is written by the
	 * kernel's hw_breakpoint init. Each is per-core, silent, and fatal on a
	 * delay -- exactly the profile of what this snapshot exists to name.
	 */
	asm volatile("mov %%cr8, %0" : "=r"(v)); s->cr8 = v;
	s->pat = co_rdmsr(0x277);
	/*
	 * MTRRdefType, the other per-core cache MSR, added after it cost 109
	 * seconds of every boot.
	 *
	 * PAT above is here because Linux's pat_init rewrote it and changed what
	 * Windows' live PTEs meant. This is the same problem one register along,
	 * and worse: bit 11 is the MTRR enable, and with it clear every byte of
	 * physical memory is uncacheable on this core -- so a host that resumes
	 * with it clear does not merely misinterpret memory types, it runs the
	 * whole of Windows with caching off.
	 *
	 * The guest reaches it through cache_disable() -> mtrr_disable() inside
	 * cache_cpu_init(), a window upstream protects with local_irq_save() and
	 * this port makes virtual. That path is now blocked on the guest side as
	 * well (arch/x86/kernel/cpu/mtrr/mtrr.c), which is the actual fix; this
	 * is here so that no future guest path can do it again silently.
	 */
	s->mtrr_def = co_rdmsr(0x2ff);
	asm volatile("mov %%dr0, %0" : "=r"(v)); s->dr0 = v;
	asm volatile("mov %%dr1, %0" : "=r"(v)); s->dr1 = v;
	asm volatile("mov %%dr2, %0" : "=r"(v)); s->dr2 = v;
	asm volatile("mov %%dr3, %0" : "=r"(v)); s->dr3 = v;
	asm volatile("mov %%dr6, %0" : "=r"(v)); s->dr6 = v;
	asm volatile("mov %%dr7, %0" : "=r"(v)); s->dr7 = v;
}

/*
 * Compare against the snapshot and report the first field that moved.
 *
 * Returns CO_HOST_FIELD_NONE when the host came back exactly as it left, which
 * is the only acceptable outcome: this runs inside a driver on the machine
 * itself, so "mostly restored" means the box dies later, somewhere else, for no
 * visible reason.
 */
static co_host_field_t co_host_verify(const co_host_snapshot_t* want,
				      unsigned long long* expected,
				      unsigned long long* actual)
{
	co_host_snapshot_t now;

#define CO_CHECK(field, id)						\
	do {								\
		if (now.field != want->field) {				\
			*expected = (unsigned long long)want->field;	\
			*actual   = (unsigned long long)now.field;	\
			return (id);					\
		}							\
	} while (0)

	co_host_snapshot(&now);

	/*
	 * The processor first. Everything below it is per CPU, so if this one
	 * has moved then the rest are being compared against a different
	 * machine's registers and their verdicts mean nothing.
	 */
	CO_CHECK(cpu,		 CO_HOST_FIELD_CPU);
	CO_CHECK(cr0,		 CO_HOST_FIELD_CR0);
	CO_CHECK(cr4,		 CO_HOST_FIELD_CR4);
	CO_CHECK(cr3,		 CO_HOST_FIELD_CR3);
	CO_CHECK(gdt_base,	 CO_HOST_FIELD_GDT_BASE);
	CO_CHECK(gdt_limit,	 CO_HOST_FIELD_GDT_LIMIT);
	CO_CHECK(idt_base,	 CO_HOST_FIELD_IDT_BASE);
	CO_CHECK(idt_limit,	 CO_HOST_FIELD_IDT_LIMIT);
	CO_CHECK(tr,		 CO_HOST_FIELD_TR);
	CO_CHECK(fs_base,	 CO_HOST_FIELD_FS_BASE);
	CO_CHECK(gs_base,	 CO_HOST_FIELD_GS_BASE);
	CO_CHECK(kernel_gs_base, CO_HOST_FIELD_KERNEL_GS_BASE);
	CO_CHECK(lstar,		 CO_HOST_FIELD_LSTAR);
	CO_CHECK(star,		 CO_HOST_FIELD_STAR);
	CO_CHECK(sfmask,	 CO_HOST_FIELD_SFMASK);
	CO_CHECK(efer,		 CO_HOST_FIELD_EFER);
	CO_CHECK(cs,		 CO_HOST_FIELD_CS);
	CO_CHECK(ss,		 CO_HOST_FIELD_SS);
	CO_CHECK(ds,		 CO_HOST_FIELD_DS);
	CO_CHECK(es,		 CO_HOST_FIELD_ES);
	CO_CHECK(fs,		 CO_HOST_FIELD_FS);
	CO_CHECK(gs,		 CO_HOST_FIELD_GS);
	CO_CHECK(cr8,		 CO_HOST_FIELD_CR8);
	CO_CHECK(pat,		 CO_HOST_FIELD_PAT);
	CO_CHECK(mtrr_def,	 CO_HOST_FIELD_MTRR_DEF);
	CO_CHECK(dr7,		 CO_HOST_FIELD_DR7);
	CO_CHECK(rflags,	 CO_HOST_FIELD_RFLAGS);
	CO_CHECK(cr2,		 CO_HOST_FIELD_CR2);
	CO_CHECK(ldt,		 CO_HOST_FIELD_LDT);
	CO_CHECK(cstar,		 CO_HOST_FIELD_CSTAR);
	CO_CHECK(sysenter_cs,	 CO_HOST_FIELD_SYSENTER_CS);
	CO_CHECK(sysenter_esp,	 CO_HOST_FIELD_SYSENTER_ESP);
	CO_CHECK(sysenter_eip,	 CO_HOST_FIELD_SYSENTER_EIP);
	CO_CHECK(dr0,		 CO_HOST_FIELD_DR0);
	CO_CHECK(dr1,		 CO_HOST_FIELD_DR1);
	CO_CHECK(dr2,		 CO_HOST_FIELD_DR2);
	CO_CHECK(dr3,		 CO_HOST_FIELD_DR3);
	CO_CHECK(dr6,		 CO_HOST_FIELD_DR6);
	CO_CHECK(xcr0,		 CO_HOST_FIELD_XCR0);

#undef CO_CHECK

	return CO_HOST_FIELD_NONE;
}

/*
 * One three-byte replay stub for each external vector:
 *
 *     int $vector
 *     ret
 *
 * The external interrupt was accepted while the guest address space was
 * current, so the local APIC already has the vector in its in-service state.
 * Once the world switch restores the host IDT/TSS/CR8, executing the same
 * vector with INT makes the processor enter Windows through its real gate.
 * The processor builds the architecture-defined frame, performs any host IST
 * switch, and IRETQ unwinds it. Windows' handler services and EOIs the original
 * hardware interrupt; RET then brings us back to the monitor.
 *
 * This is deliberately outside co_switch_full..co_switch_full_end: it executes
 * only in the host mapping and must not consume passage-page code space.
 */
asm(".text                                                        \n"
    ".balign 16                                                   \n"
    ".globl co_host_interrupt_replay_stubs                        \n"
    "co_host_interrupt_replay_stubs:                              \n"
    ".set co_replay_vector, 32                                    \n"
    ".rept 224                                                    \n"
    "    .byte 0xcd, co_replay_vector /* int $vector */           \n"
    "    ret                                                       \n"
    "    .set co_replay_vector, co_replay_vector + 1              \n"
    ".endr                                                        \n");

extern char co_host_interrupt_replay_stubs;

/*
 * Vector a running guest to its timer entry, or decide not to.
 *
 * Called only from the interrupt-replay path, where the guest is stopped with a
 * complete interrupt frame already on its stack -- the one its own IDT stub
 * saved before crossing. That frame is what makes this possible without any new
 * assembly: the hard parts of taking an interrupt (finding a kernel stack,
 * having somewhere to put the interrupted registers) are already done.
 *
 * The stub's frame, from CO_PP_GUEST_FRAME:
 *
 *   0x00..0x70  the fifteen saved general registers
 *   0x78 vector    0x80 error code
 *   0x88 RIP  0x90 CS  0x98 RFLAGS  0xa0 RSP  0xa8 SS   (pushed by the CPU)
 *
 * The five words at 0x88 are what co_guest_resume's iretq consumes. So: copy
 * them somewhere the timer entry can iretq from later, then overwrite them to
 * point at the timer entry instead. The guest resumes into a stock idtentry
 * with a valid interrupt frame beneath it and cannot tell the difference
 * between this and a real interrupt -- which is the whole design. In
 * particular error_entry reads the *saved* CS to decide whether to swapgs, so a
 * guest interrupted in userspace gets its GS base swapped exactly as hardware
 * would have arranged, and this code does not have to know about GS at all.
 *
 * Refuses in three cases, each of which would be worse than a late tick:
 *
 *   - no addresses, so the guest predates this and has no timer entry;
 *   - the guest's virtual interrupt flag is clear, meaning it believes
 *     interrupts are off. Hardware would not have delivered one either, and
 *     delivering anyway lands a tick handler in the middle of a critical
 *     section;
 *   - the guest's saved RFLAGS has IF clear, which is the same statement from
 *     the other side and costs one read to check.
 *
 * The new frame goes forty bytes below the stub's, which is stack the guest is
 * already using and has room for -- an interrupt arriving one instruction later
 * would have consumed several times as much.
 */
/*
 * Injection is held off by name, and this restores the status quo rather than
 * deciding anything new.
 *
 * The virtual-IF gate below reads co_colinux_virtual_if, a kernel .bss address
 * -- 0xffffffff83889ee0. mark_rodata_ro() splits text and rodata to 4 KB and
 * stops at __end_rodata_hpage_align, 0xffffffff82e00000, so .bss is still
 * covered by level2_kernel_pgt's original 2 MB PSE entries. Until
 * co_arch_guest_lookup_root learned to add the offset within a large page, that
 * read did not return the flag at all: it returned the eight bytes of
 * .call_sites at 0xffffffff83800ee0, which are 0xfea2920cfea29147, and bit 9 of
 * that is clear. The gate therefore answered "the guest has interrupts off" on
 * every single call, and this function has never once injected a tick.
 *
 * Correcting the walk turned it on for the first time, in the middle of an
 * unrelated bring-up, and the machine reset instantly with no bugcheck after
 * 8682 switches -- which is what synthesising an interrupt frame onto a live
 * guest stack looks like when the code doing it has never executed. The comment
 * on the function records the same sequence happening once already, one layer
 * down: "the readiness gate was an accident of a bug".
 *
 * So injection gets a rung of its own, with its own hardware verification,
 * instead of arriving as a side effect of a page-table fix. Every gate above
 * stays live, so out->ticks_injected now measures how many ticks WOULD be
 * delivered per run without touching a byte of guest memory.
 */
static const bool_t co_tick_injection_enabled = PFALSE;

static void co_arch_inject_tick(co_manager_t* manager,
				co_arch_passage_page_t* pp,
				const co_vcpu_ctl_t* ctl,
				int vcpu_index,
				unsigned long long* last,
				co_arch_boot_result_t* out)
{
	const unsigned long long period = CO_GUEST_TICK_100NS;
	unsigned long long frame_va, now, vif = 0;
	unsigned long long f[5], newsp;
	unsigned long long cr3 = pp->linuxvm_state.cr3;

	if (!ctl->tick_entry_va || !ctl->virtual_if_va)
		return;

	/*
	 * Not until the guest has idled at least once.
	 *
	 * Reaching the idle loop is proof the kernel finished starting: the
	 * clock event device is registered, trap_init has run, per-CPU state
	 * exists, and there is a scheduler to preempt. Before that there is
	 * nothing this interrupt can usefully do and a great deal it can break.
	 *
	 * This was found the hard way and only became reachable when the frame
	 * read was fixed. While that read went through the host's load-time
	 * root it failed on almost every frame, so injection quietly did
	 * nothing until the guest was well up -- the readiness gate was an
	 * accident of a bug. Reading through the guest's own CR3 made injection
	 * work everywhere, including twenty milliseconds into boot, and the
	 * first tick delivered landed in sysvec_co_timer before trap_init: page
	 * fault at sysvec_co_timer+0x61, guest dead, one tick injected.
	 *
	 * out->idle_yields is the signal because it costs nothing and cannot be
	 * faked: the guest only performs a cooperative idle yield from
	 * co_cpu_idle, which is the idle task, which runs when the run queue
	 * empties.
	 */
	if (out->idle_yields == 0)
		return;

	frame_va = pp->params[28];		/* CO_PP_GUEST_FRAME */
	if (!frame_va)
		return;

	now = co_os_monotonic_100ns();
	if (*last == 0) {
		*last = now;
		return;
	}
	if (now - *last < period)
		return;

	/*
	 * Would hardware have delivered here? The guest's cli/sti are virtual,
	 * so this flag -- not the real one -- is the answer, and it is the
	 * whole of the contract being honoured.
	 *
	 * THIS processor's slot, not the base of the array.
	 *
	 * co_colinux_virtual_if is one slot per guest processor
	 * (asm/irqflags.h), and virtual_if_va names slot 0 because slot 0 is
	 * where the loader's symbol lookup lands. Asking slot 0 whether a tick
	 * may be injected into vCPU 1 asks the wrong processor: vCPU 1 could be
	 * anywhere inside a local_irq_save() section and the answer would still
	 * be yes because vCPU 0 happens to have interrupts on. Hardware would
	 * not have delivered there, and injecting anyway synthesises an
	 * interrupt frame onto a stack that is mid-update.
	 *
	 * Unreachable while only the boot processor injected, which is why it
	 * survived: the secondary ran a lane that never called this at all.
	 */
	if (!CO_OK(co_kload_read_cr3(manager, cr3,
				     ctl->virtual_if_va +
					(unsigned long long)vcpu_index * 8,
				     (unsigned char*)&vif, sizeof(vif))) ||
	    (vif & 0x200) == 0)
		return;

	/*
	 * The frame the stub saved, read through the root the guest was
	 * actually running on -- not the host's load-time space.
	 *
	 * This distinction is the whole difference between a cooperative timer
	 * that works and one that silently never fires. An interrupt from ring
	 * 3 takes TSS.RSP0, which is the cpu_entry_area entry stack at
	 * 0xfffffe00...; cpu_entry_area is a top-level entry Linux created for
	 * itself after boot handoff and it exists only in the guest's own
	 * tables. Read through kload_space it is simply not present, the lookup
	 * returns NOT_FOUND, and this function quietly declines -- on every
	 * attempt against a userspace task, forever, while appearing to work
	 * against the few kernel contexts still on the direct map. With
	 * CONFIG_VMAP_STACK most kernel stacks are invisible that way too.
	 */
	if (!CO_OK(co_kload_read_cr3(manager, cr3, frame_va + 0x88,
				     (unsigned char*)f, sizeof(f))))
		return;

	if ((f[2] & 0x200) == 0)		/* saved RFLAGS: IF clear */
		return;

	/*
	 * Userspace only. This is the difference between a preemptible guest and
	 * a triple fault, and the reason is GS.
	 *
	 * asm_sysvec_co_timer is a stock idtentry, and error_entry decides
	 * whether to swapgs by looking at the interrupted CS: ring 3 means the
	 * user's GS base is loaded and must be swapped, ring 0 means the
	 * kernel's already is. That inference is true for the frames hardware
	 * delivers and false for some of the instants this host can choose,
	 * because the guest runs with real interrupts enabled everywhere --
	 * including inside its own entry sequences.
	 *
	 * The fatal one is SYSCALL. It loads a kernel CS but does not touch GS;
	 * entry_SYSCALL_64 swaps it a few instructions later. Nothing on the
	 * syscall path clears co_colinux_virtual_if either -- it is handled in
	 * irqentry_enter/exit and exit_to_user_mode, and a syscall goes through
	 * none of them -- so the flag still reads 0x200 from userspace and the
	 * gate above says yes. Inject there and error_entry sees ring 0, does
	 * not swap, and the kernel runs on the user's GS base: every per-CPU
	 * access lands somewhere arbitrary, it faults, the fault handler needs
	 * GS as well, and the machine is gone with no bugcheck and nothing to
	 * read.
	 *
	 * On real hardware that window is covered because the entry asm runs
	 * with interrupts off. Here it cannot be, so the window is covered from
	 * this side instead: over a ring 3 frame the answer is never ambiguous.
	 *
	 * Nothing is really lost. Preemption is needed for a task that will not
	 * yield, and that task is in userspace by definition -- a kernel path
	 * that never returns to user and never yields is a bug of its own.
	 * Kernel-mode time still arrives through the exit-to-user drain and the
	 * cooperative yields, as it always did.
	 *
	 * Found by Steam: thousands of syscalls a second across several threads
	 * against a thousand injections a second, and it triple faulted while
	 * installing. The single-threaded spinner that validated this makes no
	 * syscalls at all and never touched the window.
	 */
	if ((f[1] & 3) != 3)			/* saved CS: not ring 3 */
		return;

	/* Counted, not delivered. See co_tick_injection_enabled above. */
	if (!co_tick_injection_enabled) {
		*last = now;
		out->ticks_injected++;
		return;
	}

	newsp = (frame_va - 0x40) & ~0xfULL;

	if (!CO_OK(co_kload_write_cr3(manager, cr3, newsp,
				      (const unsigned char*)f, sizeof(f))))
		return;

	/*
	 * Enter the handler the way a gate would: kernel CS, interrupts off,
	 * the stack pointing at the frame that returns to the interrupted
	 * context -- and the kernel's SS, which is the part that is easy to get
	 * wrong and fatal when it is.
	 *
	 * iretq in long mode reloads SS whatever the privilege change, and it
	 * requires the selector's RPL to match the CPL being returned to. The
	 * interrupted context here is usually userspace, so the SS the stub
	 * saved has RPL 3; pairing it with a ring-0 CS is a general protection
	 * fault on the iretq itself, before a single instruction of the handler
	 * runs. Both selectors have to come from the same place, and the
	 * crossing already recorded the guest kernel's pair.
	 */
	f[0] = ctl->tick_entry_va;	/* RIP   */
	f[1] = pp->linuxvm_state.cs;	/* CS    -- the guest's kernel selector */
	f[2] &= ~0x200ULL;		/* FLAGS -- IF off, as a gate leaves it */
	f[3] = newsp;			/* RSP   */
	f[4] = pp->linuxvm_state.ss;	/* SS    -- must match CS's privilege */

	if (!CO_OK(co_kload_write_cr3(manager, cr3, frame_va + 0x88,
				      (const unsigned char*)f, sizeof(f))))
		return;

	*last = now;
	out->ticks_injected++;
}

bool_t co_arch_forward_host_interrupt(void* host_idt, unsigned long long vector)
{
	struct co_x86_64_gate* gate;
	void* stub;

	if (vector < 32 || vector > 255)
		return PFALSE;

	/*
	 * Vectors 0x20-0x2f are Windows' own software-service gates, and
	 * replaying one is not forwarding an interrupt, it is calling a system
	 * service by hand:
	 *
	 *   0x2a KiGetTickCount   0x2b KiCallbackReturn
	 *   0x2c KiRaiseAssertion 0x2d KiDebugServiceTrap
	 *
	 * int 0x2d raises STATUS_BREAKPOINT, and with no kernel debugger
	 * attached that is an unhandled kernel exception -- bugcheck 0x3B,
	 * SYSTEM_SERVICE_EXCEPTION, first parameter 0x80000003. That is exactly
	 * the bugcheck this guard was written for, and 0x2c would produce its
	 * assertion twin.
	 *
	 * No hardware interrupt is ever delivered on these vectors: the HAL
	 * assigns device interrupts from 0x30 up. So capturing one means
	 * something other than an interrupt happened -- the guest executed a
	 * software int, or a vector was recorded wrongly -- and either is worth
	 * stopping to report rather than executing blind.
	 */
	if (vector >= 0x20 && vector <= 0x2f) {
		co_debug_error("boot: refusing to replay vector 0x%llx -- that is a "
			       "Windows software-service gate, not a hardware "
			       "interrupt", vector);
		return PFALSE;
	}

	/*
	 * The LIVE IDTR, not the copy the crossing saved.
	 *
	 * host_state.idt was sidt'd on the way out of the host and describes the
	 * processor the crossing happened on. This runs AFTER the monitor loop
	 * has restored the host's flags, so a Windows interrupt may already have
	 * arrived and rescheduled this thread onto another processor -- whose
	 * IDT is a different table. The int below goes through the live IDTR
	 * either way; it was only this presence check that was consulting the
	 * wrong processor's gates.
	 *
	 * It matters more than a stale read normally would, because this is the
	 * last thing in either loop that assumed a vCPU thread stays on the core
	 * it was put on. With the migration kill removed (see the crossing loop)
	 * that assumption is deliberately no longer true.
	 */
	{
		struct { unsigned short limit; unsigned long long base; }
			__attribute__((packed)) dt;

		asm volatile("sidt %0" : "=m"(dt));
		if (dt.base == 0)
			return PFALSE;

		gate = &((struct co_x86_64_gate*)(size_t)dt.base)[vector];
	}
	(void)host_idt;		/* kept in the signature as a tripwire only */

	if (!(gate->flags & 0x8000))
		return PFALSE;

	stub = (void*)((unsigned char*)&co_host_interrupt_replay_stubs
		       + (vector - 32) * 3);

	/*
	 * Keep nested interrupts out between the captured interrupt and its
	 * Windows dispatch. INT ignores IF, enters through the host's real IDT,
	 * and returns with IF still clear; restore the caller's exact flags only
	 * after the replay stub has returned on the normal host stack.
	 */
	asm volatile(
	    "    pushfq"			"\n"
	    "    popq %%r11"			"\n"
	    "    cli"				"\n"
	    "    call *%0"			"\n"
	    "    pushq %%r11"			"\n"
	    "    popfq"				"\n"
	    : : "r"(stub) : "r11", "memory", "cc");

	return PTRUE;
}

/*
 * Is a monitor loop running, and should it stop?
 *
 * The loop holds a thread inside the driver for as long as the guest runs --
 * up to fifteen minutes with a terminal attached. Killing the daemon does not
 * end it: TerminateProcess cannot interrupt a thread that is already in kernel
 * mode, so the process dies, its handles close, the driver unloads, and this
 * loop carries on executing driver code whose state has just been freed. The
 * first thing it touches is the debug system, on the next co_debug -- which is
 * a write into freed pool, and bugcheck 0xD5 under Driver Verifier.
 *
 * So unload asks the loop to stop and waits for it to say it has.
 */
/*
 * A guest processor.
 *
 * Everything a crossing owns, gathered in one place and indexed, so that the
 * step from one guest CPU to several is adding entries rather than finding
 * the globals. The rule that decides membership is simple: if the switch
 * saves it, restores it, or writes it while the guest runs, it belongs to
 * exactly one vCPU and belongs here.
 *
 * What is deliberately NOT here is the monitor loop's own locals -- deadline,
 * tick_last, idle_run, the frame buffer, the counters. Those live on the
 * stack of the thread running the loop, and a second vCPU is a second thread
 * with a stack of its own, so they are already per-vCPU for free. Moving them
 * into this struct would make them look shared and buy nothing.
 *
 * The guest address space is NOT here either, and that is not an oversight:
 * one guest has one CR3, and vCPUs of the same guest share it. Per-vCPU is
 * the passage page, not the space it is mapped into.
 */
typedef struct {
	int			index;		/* which vCPU this is */
	volatile int		active;		/* inside a crossing loop right now */
	unsigned long		host_cpu;	/* the processor it is pinned to */
	co_arch_passage_page_t*	pp;		/* its own passage page */
	/*
	 * The guest's stable address space, published by vCPU 0 once the
	 * kernel's own tables have been adopted.
	 *
	 * Not the live CR3 out of linuxvm_state: that follows whatever process
	 * Linux last scheduled, and a secondary processor must not be pointed
	 * at a per-process table that can be freed underneath it. The adopted
	 * kernel root is the one that does not move, and every process root
	 * shares the kernel half with it anyway -- which is what makes the
	 * passage pages reachable from either.
	 */
	unsigned long long	guest_cr3;
	/*
	 * The initial state a secondary is started with, handed over by the
	 * guest through CO_OPERATION_START_VCPU. This is the cooperative
	 * equivalent of INIT-SIPI: instead of a processor being reset and
	 * fetching from a trampoline, the host is told where to begin, on what
	 * stack, in which address space and with which per-CPU base -- the
	 * same set Xen's cpu_initialize_context() hands its hypervisor.
	 *
	 * start_pending is written by vCPU 0 inside its crossing loop and read
	 * by the waiting AP thread, with a doorbell between them, so it needs
	 * no lock: one writer, one reader, and the reader only ever waits.
	 */
	volatile int		start_pending;
	unsigned long long	start_rip;
	unsigned long long	start_rsp;
	unsigned long long	start_cr3;
	unsigned long long	start_gs_base;
	/*
	 * Where the guest's co_colinux_passage_page[] array begins, published
	 * by vCPU 0. A secondary has to be able to find the crossing before it
	 * can make one, and it finds it by reading slot N of that array -- so
	 * the host writes its own passage page into slot N before letting it
	 * run. There is no other channel: the AP's first instruction is guest
	 * code, and by then the answer has to already be in memory.
	 */
	unsigned long long	passage_symbol_va;
	/*
	 * Every distinct host vector this vCPU has replayed, once. Per-vCPU
	 * rather than shared because the question it answers -- was this
	 * processor handed something the host never raises -- is asked of a
	 * processor. A shared table would let the first vCPU to see a vector
	 * silence the report for every other one.
	 */
	unsigned char		seen_vector[256];
} co_vcpu_t;

static co_vcpu_t	   co_vcpu[CO_MAX_VCPUS];
/*
 * Broadcast, not per-vCPU: KSTOP and driver unload mean "all of you, now".
 * Monotonic within a run and read by volatile loads on every crossing, so it
 * needs no lock -- the same arrangement the single boot loop always had.
 */
static volatile int	   vcpu_abort_all;

/*
 * Secondary passage pages, allocated and mapped before the guest's tables are
 * adopted so the graft covers them.
 *
 * Allocating one when the secondary starts does not work, and the reason is
 * the graft: co_kload_adopt_kernel_tables copies kload_space's TOP-LEVEL
 * entries into the kernel's table, once. A passage page mapped into
 * kload_space afterwards is reachable from the kernel's root only if it
 * happens to fall in a slot that graft already copied -- and nonpaged pool is
 * under no obligation to stay in one 512 GB slot. When it did not, the
 * secondary came up, printed that it was up, and page faulted on the first
 * instruction that touched its own passage page, in co_cpu_idle.
 *
 * The alternative -- writing the missing entry into the live table -- resets
 * the machine with no bugcheck, because a processor is walking that structure
 * at the time. So the pages are made before the guest owns anything, which is
 * the one moment when adding to kload_space is free.
 */
static co_arch_passage_page_t* ap_passage[CO_MAX_VCPUS];

/*
 * The start request, kept outside co_vcpu[].
 *
 * A secondary's thread now polls -- it claims a slot, asks whether the guest
 * has requested this processor, and releases the slot again if not. Claiming
 * clears the slot, so a request that arrived between two polls would be wiped
 * by the very next one. The request has to outlive the slot, so it lives here:
 * written once by the boot processor inside its crossing loop, read by the
 * thread that will become that processor.
 */
/*
 * The policy every crossing loop consults, built once by the boot processor.
 *
 * This replaces cobd_async_enabled and cobd_io_va_shared, which were two
 * file-scope copies of two fields of the boot block, added one at a time as
 * each secondary failure named them. The struct carries everything a loop
 * reads per iteration, so the next field a secondary turns out to need is added
 * in one place rather than discovered on hardware.
 *
 * Written before any secondary exists and read-only afterwards, which is what
 * lets both loops hold a pointer to it with no lock.
 */
static co_vcpu_ctl_t	  vcpu_ctl;

/*
 * The one crossing loop, shared by the boot processor and every secondary.
 * Defined below co_arch_boot_prepare(); declared here because a secondary
 * reaches it from co_arch_test_smp_lane(), which comes first in this file.
 */
static co_rc_t co_arch_vcpu_run(co_manager_t* manager, co_vcpu_t* vcpu,
				const co_vcpu_ctl_t* ctl,
				co_arch_boot_result_t* out);

/*
 * A secondary's result block, one per vCPU and NOT on the stack.
 *
 * The shared loop reports into a co_arch_boot_result_t, which is about 2.8 KB
 * -- console_text[2048] alone -- and co_arch_test_smp_lane() runs on an ioctl
 * thread inside the driver, alongside the loop's own frame buffer and host
 * snapshots. Putting one there is a large fraction of a kernel stack spent to
 * hold a report whose interesting fields are copied straight back out.
 *
 * One per index rather than one shared, because two secondaries run at once
 * and each owns exactly its own slot for the life of its run.
 */
static co_arch_boot_result_t ap_result_slot[CO_MAX_VCPUS];

/*
 * The boot processor's guest XCR0, captured when it asks for a secondary.
 *
 * XCR0 rides the crossing per passage page, so each vCPU carries its own --
 * and a secondary's comes from whatever its freshly built page held, not from
 * what the guest configured on vCPU 0. Userspace decides ONCE, from CPUID,
 * whether it may use AVX and friends; a thread that decided yes on the boot
 * processor and is then migrated to a secondary runs the same instruction
 * against a different XCR0 and takes #UD.
 *
 * Captured rather than read live because the boot loop's passage page is a
 * local: by the time START_VCPU arrives the guest has long since run its FPU
 * init, so the value saved on that exit is the one the guest is really using.
 */
static unsigned long long vcpu0_xcr0;
static unsigned long long vcpu0_cr4;

static volatile int	  ap_start_pending[CO_MAX_VCPUS];
static unsigned long long ap_start_rip[CO_MAX_VCPUS];
static unsigned long long ap_start_rsp[CO_MAX_VCPUS];
static unsigned long long ap_start_cr3[CO_MAX_VCPUS];
static unsigned long long ap_start_gs[CO_MAX_VCPUS];

static co_vcpu_t* co_vcpu_claim(int index)
{
	co_vcpu_t* vcpu;

	if (index < 0 || index >= CO_MAX_VCPUS)
		return NULL;

	vcpu = &co_vcpu[index];
	co_memset(vcpu, 0, sizeof(*vcpu));
	vcpu->index    = index;
	vcpu->host_cpu = co_os_current_cpu();

	/*
	 * The BOOT processor clears a stale abort -- not "whoever is first".
	 *
	 * "First in" was written when only the boot loop ever claimed a slot.
	 * A secondary's thread now starts BEFORE the guest boots, so it became
	 * first, and by the time vCPU 0 claimed slot 0 co_arch_boot_running()
	 * was already true -- so the boot processor never cleared the abort
	 * left by the previous run, and died partway through booting with the
	 * secondary still parked and no guest instruction executed. That is
	 * the failure that was mistaken for "a thread waiting inside the
	 * driver destabilises the boot processor".
	 *
	 * A secondary must still never clear it: it would revive a guest that
	 * KSTOP is in the middle of ending.
	 */
	if (index == 0)
		vcpu_abort_all = 0;

	vcpu->active = 1;
	return vcpu;
}

static void co_vcpu_release(co_vcpu_t* vcpu)
{
	vcpu->active = 0;
	vcpu->pp     = NULL;
}

/*
 * Is a host processor already carrying a vCPU?
 *
 * Two vCPUs pinned to one core is not slow, it is a deadlock: a crossing
 * loop holds its processor for as long as the guest runs, so the second
 * thread never gets to run and the first waits for work the second was
 * supposed to do. The core map is small and read while a run is starting,
 * which is not a hot path, so a linear scan is the whole mechanism.
 */
int co_arch_vcpu_core_taken(unsigned long cpu)
{
	int i;

	for (i = 0; i < CO_MAX_VCPUS; i++)
		if (co_vcpu[i].active && co_vcpu[i].host_cpu == cpu)
			return 1;

	return 0;
}

void co_arch_boot_abort(void)
{
	vcpu_abort_all = 1;
	/*
	 * And wake everyone, because the flag is only read at the top of a
	 * crossing loop and a parked vCPU is not at the top of anything.
	 *
	 * The idle wait is indefinite now, so this is the ONLY thing that ends
	 * it. Every way a run can be stopped arrives here -- KSTOP from
	 * stop.bat, driver unload when the last handle closes, and the guest's
	 * own TERMINATE -- so all three are covered by this one line. Before it,
	 * each of them depended on a poll timeout expiring, which is the same
	 * accident the teardown join was relying on.
	 */
	co_os_idle_wake_all();
}

int co_arch_boot_running(void)
{
	int i;

	for (i = 0; i < CO_MAX_VCPUS; i++)
		if (co_vcpu[i].active)
			return 1;

	return 0;
}

/*
 * The SMP spike: one lane of the concurrent crossing test.
 *
 * Two callers run this at once, each pinned to a different processor by the
 * ioctl handler, each with a passage page of its own from co_setup_guest_page
 * -- which already builds everything per-call: tables, GDT, TSS, IST stack,
 * stubs, IDT. Nothing here is shared between lanes on purpose; the test
 * exists to prove that nothing needs to be.
 *
 * The guest is co_switch_guest_loop entered through co_boot_shim, so it runs
 * with real IF set, exactly as the booted kernel does. That is the point:
 * hardware interrupts on both processors then vector through two different
 * guest IDTs into two different stubs and are replayed into the live Windows
 * IDT from two threads at once -- the concurrency the boot path has never
 * once exercised, bought here for the price of a test ioctl.
 *
 * The MSR sentinels are the other half. Each lane plants lane-distinct values
 * in its guest state's FS_BASE/GS_BASE/KERNEL_GS_BASE/LSTAR; the switch loads
 * them entering the guest and saves the live values back leaving it. The
 * guest never writes those MSRs, so any drift means one lane's crossing
 * touched the other lane's state -- the exact failure that would surface as
 * unexplainable corruption under a real second vCPU. The host side needs no
 * separate check: co_host_verify already compares the live MSRs against a
 * snapshot taken before every crossing, per lane, on the lane's own stack.
 */
static unsigned long long co_smp_sentinel(int lane, int which)
{
	/*
	 * Canonical by construction (bit 47 clear), distinct per lane and per
	 * MSR, and unlike anything Windows or the host driver would ever put
	 * in a segment base -- so a bleed cannot be mistaken for a legitimate
	 * value, and a legitimate value cannot be mistaken for a bleed.
	 */
	return 0x0000500000000000ULL |
	       ((unsigned long long)(lane + 1) << 36) |
	       ((unsigned long long)(which + 1) << 20);
}

co_rc_t co_arch_test_smp_lane(co_manager_t* manager, co_arch_smp_test_t* out,
			      int lane, long long iterations, int join_guest)
{
	co_arch_switch_test_t setup;
	co_arch_passage_page_t* pp;
	co_vcpu_t* vcpu;
	co_switch_full_fn fn;
	co_host_snapshot_t host_was;
	unsigned long long resume_rip, ist_top, loop_va;
	unsigned long long host_flags;
	long long done;
	int pages = sizeof(co_arch_passage_page_t) / CO_ARCH_PAGE_SIZE;

	co_memset(out, 0, sizeof(*out));
	out->supported = PTRUE;
	out->lane      = lane;

	/*
	 * Zero is not a small number here, it is a different mode: wait for
	 * the guest to start this processor. Only the trivial loop's count
	 * gets clamped.
	 */
	if (iterations != 0 && (iterations < 1 || iterations > 100000000LL))
		iterations = 1000000;
	out->iterations = iterations;

	/*
	 * A lane IS a vCPU: same slot, same claim, same abort flag, same
	 * per-vCPU replayed-vector table as the monitor loop uses. That is
	 * deliberate rather than tidy -- it means this test exercises the
	 * structure a second guest processor will run on, concurrently, and
	 * that KSTOP and driver unload need to know about one kind of thing
	 * holding a thread inside the driver rather than two.
	 */
	vcpu = co_vcpu_claim(lane);
	if (vcpu == NULL)
		return CO_RC(INVALID_PARAMETER);

	co_memset(&setup, 0, sizeof(setup));

	/*
	 * A real secondary uses the passage page the boot path made for it
	 * before the guest adopted its tables -- that is what makes it
	 * reachable from the guest's own root. Only the trivial test loop,
	 * which runs in a space of its own, allocates one here.
	 */
	if (join_guest && iterations == 0) {
		/*
		 * The page was allocated and mapped at boot so the graft covers
		 * it, but it must be BUILT here, on this thread, because
		 * everything co_setup_guest_page_on() captures belongs to the
		 * processor it runs on. Built on the boot core and entered from
		 * this one, it restores the boot core's GDTR, IDTR and TR onto
		 * this core when the guest hands back -- which is not a fault
		 * that can be reported, it is a reset, or a core that stops
		 * taking the clock. That is the pinning rule this port already
		 * has, applied to a second processor for the first time.
		 */
		if (ap_passage[lane] == NULL) {
			co_debug_error("vcpu %d: no passage page was made for it at "
				       "boot -- refusing to start it", lane);
			out->never_started = PTRUE;
			co_vcpu_release(vcpu);
			return CO_RC(ERROR);
		}
		/*
		 * Taken as-is for now; BUILT after the wait, not before.
		 *
		 * Building it here means running co_arch_save_state() and
		 * rewriting fifteen pages on this core while the boot processor
		 * is crossing on another -- work this thread has no reason to
		 * do until it is actually asked to start. Whatever it disturbs,
		 * it disturbs for the whole length of the wait, which can be
		 * the entire boot. Doing it after the request keeps this thread
		 * asleep until the guest wants it, and keeps the build on the
		 * core that will enter it.
		 */
		pp = ap_passage[lane];
	} else {
		pp = co_setup_guest_page(&setup,
			(unsigned long)(&co_boot_shim - &co_switch_full));
		if (pp == NULL) {
			co_vcpu_release(vcpu);
			return CO_RC(ERROR);
		}
	}
	vcpu->pp = pp;

	/*
	 * Join the running guest's address space, rather than the private one
	 * co_setup_guest_page built for this passage page.
	 *
	 * This is the property a real secondary processor needs and the one
	 * --test-smp does not exercise: two vCPUs crossing into the SAME CR3.
	 * Each keeps its own passage page -- its own saved state, params, FPU
	 * area, IST stack, TSS and GDT -- but they share the page tables, which
	 * is what makes them processors of one machine instead of two guests.
	 *
	 * Mapping into kload_space is what makes it reachable. The boot path
	 * grafts kload_space's top-level entries into the kernel's own table
	 * (co_kload_adopt_kernel_tables), so a page added under an
	 * already-grafted slot is visible from the kernel's CR3 without
	 * touching the kernel's tables at all. A passage page that landed in a
	 * slot the graft never saw would NOT be reachable, and the preflight
	 * below is what catches that rather than the guest discovering it by
	 * failing to fetch its first instruction.
	 */
	/*
	 * Map before waiting to be started, never after.
	 *
	 * Mapping populates kload_space, whose top-level entries the boot path
	 * grafted into the kernel's table -- so it touches structures the guest
	 * walks. Doing it here, before this thread parks, means it happens
	 * while the guest is idling in its own steady state. Doing it after the
	 * wait means doing it at the instant START_VCPU arrives, which is
	 * inside smp_init(), with the boot processor mid-crossing. That
	 * reordering is what turned a working bring-up into a triple fault:
	 * the last build that reached "CPU 1 is up" mapped first and waited
	 * second.
	 *
	 * A real secondary skips this entirely -- its page was mapped at boot,
	 * before the graft, so nothing needs touching now at all.
	 */
	if (join_guest && iterations != 0) {
		co_arch_guest_space_t* space = co_kload_space();
		unsigned long long guest_cr3 = co_vcpu[0].guest_cr3;
		int page;

		if (space == NULL || !co_vcpu[0].active || guest_cr3 == 0) {
			co_debug_error("vcpu %d: no running guest to join "
				       "(space %p, vcpu0 active %d, cr3 0x%llx)",
				       lane, (void*)space, co_vcpu[0].active,
				       guest_cr3);
			if (pp != ap_passage[lane]) co_os_free_exec_pages(pp, pages);
			co_vcpu_release(vcpu);
			return CO_RC(ERROR);
		}

		for (page = 0; page < pages; page++) {
			unsigned char* q = (unsigned char*)pp + page * CO_ARCH_PAGE_SIZE;

			if (!CO_OK(co_arch_guest_map(manager, space,
						     (unsigned long long)(size_t)q,
						     co_os_virt_to_phys(q),
						     _KERNPG_TABLE))) {
				co_debug_error("vcpu %d: could not map its passage "
					       "page into the guest's space", lane);
				if (pp != ap_passage[lane]) co_os_free_exec_pages(pp, pages);
				co_vcpu_release(vcpu);
				return CO_RC(ERROR);
			}
		}

		pp->linuxvm_state.cr3 = guest_cr3;
		co_debug("vcpu %d: joined the guest's address space, cr3 0x%llx",
			 lane, guest_cr3);
	}

	/*
	 * co_boot_shim executes sti and jumps to params[20]; step (params[29])
	 * is zero, so TF stays clear and the guest free-runs with interrupts
	 * on -- which is what routes this processor's clock through the guest
	 * IDT while the loop runs.
	 */
	loop_va = (unsigned long long)(size_t)pp->code
		+ (unsigned long)(&co_switch_guest_loop - &co_switch_full);
	/*
	 * The trivial lane's scaffolding, and only for the trivial lane.
	 *
	 * params[20] here is co_switch_guest_loop -- the test loop's entry --
	 * and the four sentinels exist to prove one lane's MSRs never appear in
	 * another's. A real secondary overwrites all six from ap_start_* once
	 * the request is seen, so for it these are not merely useless: they run
	 * on EVERY poll, and a real secondary's page is ap_passage[lane], which
	 * was mapped into the guest's address space at boot and is walked by the
	 * live guest. That is a write into a running machine's memory, once per
	 * poll, for the whole of its boot, to install values that are wrong and
	 * are then discarded.
	 *
	 * iterations == 0 is what distinguishes the two, exactly as it does at
	 * the passage-page choice above and the wait below.
	 */
	if (iterations != 0) {
		pp->params[20] = loop_va;

		pp->linuxvm_state.fs_base        = co_smp_sentinel(lane, 0);
		pp->linuxvm_state.gs_base        = co_smp_sentinel(lane, 1);
		pp->linuxvm_state.kernel_gs_base = co_smp_sentinel(lane, 2);
		pp->linuxvm_state.lstar          = co_smp_sentinel(lane, 3);
	}

	/*
	 * Iterations of zero means "this is a real secondary": wait for the
	 * guest to say where to begin, rather than running the trivial loop.
	 *
	 * The wait is bounded. A guest that never issues START_VCPU is the
	 * normal case for every build before the kernel could -- and an ioctl
	 * thread parked in the driver forever is the failure this port has
	 * paid for more than once.
	 */
	if (join_guest && iterations == 0) {
		int spins;

		/*
		 * Not started yet? Return, and let the caller ask again.
		 *
		 * This used to block here for the whole of the guest's boot,
		 * and that is the thing that was wrong. S0 proved two crossing
		 * threads coexist happily -- ten million crossings a lane --
		 * but in S0 each thread entered its ioctl and began work at
		 * once. Nothing ever sat inside the driver waiting. Here a
		 * thread claimed a vCPU slot, took a processor and then slept
		 * in kernel context for seconds on end, and the boot processor
		 * died partway through booting every single time, without this
		 * thread having executed one guest instruction.
		 *
		 * So the waiting moves out to the daemon, which is where a
		 * thread with nothing to do belongs. The ioctl now answers one
		 * question -- has the guest asked for this processor yet --
		 * and either starts it or returns immediately.
		 */
		out->waited_for_start = PTRUE;
		(void)spins;

		/*
		 * The one thing the log cannot currently say: did this thread
		 * survive from the KVCPU_RUN debug to here, and what does it see?
		 *
		 * Everything between those two points is invisible -- the claim,
		 * the pin, taking ap_passage[lane] UNBUILT, and the params[20] and
		 * sentinel writes above, which go into a page mapped into the live
		 * guest on every poll. One line separates three cases.
		 */
		co_debug("vcpu %d: poll on host processor %lu -- pending %d, pp 0x%llx",
			 lane, (unsigned long)co_os_current_cpu(),
			 ap_start_pending[lane],
			 (unsigned long long)(size_t)pp);

		if (!ap_start_pending[lane]) {
			out->never_started = PTRUE;
			if (pp != ap_passage[lane]) co_os_free_exec_pages(pp, pages);
			co_vcpu_release(vcpu);
			return CO_RC(OK);
		}

		/*
		 * The state the guest chose. gs_base is the AP's per-CPU base
		 * and is the one field that cannot be guessed: every per-CPU
		 * access the new processor makes, from its first instruction,
		 * resolves through it -- including co_colinux_vcpu_id() itself,
		 * which is how the guest works out which passage page is its
		 * own.
		 */
		/*
		 * Take the processor now, not while waiting.
		 *
		 * co_vcpu_claim() recorded which core this thread was on when
		 * it claimed the slot, and the manager chose that core for it;
		 * re-pinning here makes it true again after a wait during which
		 * the scheduler was free to move this thread anywhere. From
		 * this point everything below -- building the page, and every
		 * crossing after it -- happens on one processor, which is the
		 * property the whole switch depends on.
		 */
		/* Pair with the writer's barrier: flag seen, now read payload. */
		asm volatile("" ::: "memory");

		/*
		 * From here this call is committed to entering, so it is no
		 * longer a "not yet" answer. Everything before this point
		 * leaves never_started set, which is what tells the daemon to
		 * ask again rather than retire the thread.
		 */
		out->never_started = PFALSE;

		/*
		 * Consumed. A request is served once; leaving it set lets a
		 * later poll -- or a later run -- enter on it again.
		 */
		ap_start_pending[lane] = 0;

		co_debug("vcpu %d: request seen -- rip 0x%llx, rsp 0x%llx, "
			 "cr3 0x%llx, page 0x%llx", lane, ap_start_rip[lane],
			 ap_start_rsp[lane], ap_start_cr3[lane],
			 (unsigned long long)(size_t)pp);

		/*
		 * Now build it, on this core, having been asked to start.
		 * co_arch_save_state() inside captures the descriptor tables
		 * and MSRs of the processor that will actually enter the guest
		 * and be restored on the way out.
		 */
		if (co_setup_guest_page_on(&setup,
			(unsigned long)(&co_boot_shim - &co_switch_full),
			pp) == NULL) {
			co_debug_error("vcpu %d: could not build its passage page",
				       lane);
			out->never_started = PTRUE;
			co_vcpu_release(vcpu);
			return CO_RC(ERROR);
		}

		/*
		 * Through the boot shim, exactly as the boot processor is
		 * entered -- params[20], not return_rip.
		 *
		 * co_setup_guest_page() already pointed return_rip at
		 * co_boot_shim; the shim executes sti and jumps to params[20].
		 * Overwriting return_rip with the kernel entry instead skips
		 * the shim altogether, so the new processor begins with real
		 * interrupts still masked and none of the entry contract the
		 * rest of the switch assumes. That is what the resets were:
		 * the guest reached its first instruction in a state nothing
		 * downstream was written for.
		 */
		/*
		 * The root vCPU 0 published, not the one the guest named.
		 *
		 * bac33be7 settled this and the AP path quietly undid it: a
		 * secondary must not be pointed at a per-process table, which
		 * is whatever Linux last scheduled and can be freed underneath
		 * it. co_vcpu[0].guest_cr3 is the adopted kernel root, which
		 * does not move. The guest's value is accepted only if it
		 * agrees; disagreeing means the guest asked for something this
		 * host will not do, and saying so beats entering it.
		 */
		if (ap_start_cr3[lane] != co_vcpu[0].guest_cr3) {
			co_debug_error("vcpu %d: guest asked for cr3 0x%llx but the "
				       "published kernel root is 0x%llx -- refusing",
				       lane, ap_start_cr3[lane], co_vcpu[0].guest_cr3);
			out->never_started = PTRUE;
			co_vcpu_release(vcpu);
			return CO_RC(ERROR);
		}

		pp->params[20]                   = ap_start_rip[lane];
		pp->params[29]                   = 0;	/* free-running, not stepped */
		pp->linuxvm_state.rsp            = ap_start_rsp[lane];
		pp->linuxvm_state.cr3            = co_vcpu[0].guest_cr3;
		pp->linuxvm_state.gs_base        = ap_start_gs[lane];
		pp->linuxvm_state.kernel_gs_base = 0;

		/*
		 * The boot processor's extended-state configuration, because a
		 * task does not renegotiate it when the scheduler moves it.
		 *
		 * XCR0 rides the crossing per passage page, so each vCPU has
		 * one of its own -- and a secondary's came from whatever its
		 * freshly built page happened to hold, not from what vCPU 0 is
		 * running with. Userspace decides once, from CPUID, whether it
		 * may use AVX and friends; a thread that decided yes on the
		 * boot processor and is then migrated here executes the same
		 * instruction against a different XCR0 and takes #UD.
		 *
		 * It shows up in the crypto libraries first because they are
		 * the ones that dispatch on CPU features at run time: "trap
		 * invalid opcode ... in libcrypto.so.3" and the same in
		 * libgnutls, on every process that negotiates anything. It
		 * cannot happen with one processor, which is why it arrived
		 * with the second one.
		 */
		/*
		 * CR4 above all: bit 18 is OSXSAVE, and CPUID mirrors it.
		 *
		 * The switch restores CR4 per passage page, and a secondary's
		 * page is built before that processor has run fpu__init_cpu()
		 * -- so its snapshot need not carry the bits the boot
		 * processor is running with. Userspace asks CPUID once whether
		 * it may use AVX, and CPUID.1:ECX.27 is CR4.OSXSAVE: a task
		 * that asked on vCPU 0 and is then migrated to a secondary
		 * where the bit is clear executes vzeroupper and takes #UD.
		 *
		 * Measured exactly that way: a one-processor boot reaches
		 * userspace with no traps at all, and every two-processor boot
		 * that reaches userspace traps in libcrypto, libgnutls and
		 * libQt6Core -- the three libraries that dispatch on CPU
		 * features at run time. The faulting byte is c5 f8 77.
		 */
		if (vcpu0_cr4)
			pp->linuxvm_state.cr4 = vcpu0_cr4;
		if (vcpu0_xcr0)
			pp->linuxvm_state.xcr0 = vcpu0_xcr0;
		/*
		 * And clear the test sentinels planted above. They exist to
		 * prove one lane's MSRs never appear in another's; a real
		 * secondary must start with the values a processor entering
		 * the kernel would have, and cpu_init() writes LSTAR itself.
		 */
		pp->linuxvm_state.fs_base        = 0;
		pp->linuxvm_state.lstar          = 0;
		out->msr_ok                      = PTRUE;

		/*
		 * Put this passage page where the new processor will look for
		 * it: co_colinux_passage_page[lane], written through the
		 * guest's own tables. Without it the AP's first crossing
		 * dereferences a null and the machine is gone with nothing to
		 * read -- it cannot be told any later, because by then it is
		 * already running guest code.
		 */
		co_debug("vcpu %d: page built, publishing its address", lane);



		{
			unsigned long long slot = co_vcpu[0].passage_symbol_va
				+ (unsigned long long)lane * 8;
			unsigned long long pp_va = (unsigned long long)(size_t)pp;

			/*
			 * Through kload_space, not the guest's root, and this is
			 * the mechanism rather than a workaround: the boot
			 * processor's own slot is published exactly this way (see
			 * co_write_guest_u64 on in->passage_symbol_va), and it is
			 * the one path here with a boot's worth of evidence behind
			 * it. The host mapped the whole image with 4 KB PTEs, so
			 * this walk never touches the large-page case at all.
			 *
			 * Both roots name the same frames for an image VA -- the
			 * host's PTE is kva - __START_KERNEL_map + phys_base
			 * (kload_kva_pa) and the fixed-up level2_kernel_pgt leaf
			 * is the 2 MB-aligned form of the same arithmetic -- so
			 * what is written here is what the secondary reads.
			 */
			if (!co_vcpu[0].passage_symbol_va ||
			    !CO_OK(co_kload_write(manager, slot,
						  (const unsigned char*)&pp_va,
						  sizeof(pp_va)))) {
				co_debug_error("vcpu %d: could not publish its passage "
					       "page at 0x%llx -- refusing to start it",
					       lane, slot);
				out->never_started = PTRUE;
				if (pp != ap_passage[lane]) co_os_free_exec_pages(pp, pages);
				co_vcpu_release(vcpu);
				return CO_RC(ERROR);
			}
		}
		/*
		 * TEMPORARY, and AFTER the write -- which is the whole point.
		 * Sited before it, this read returns the .bss slot's zero and
		 * the .call_sites bytes behind the truncating guest-root walk,
		 * which is exactly what a correct pre-publish state looks like
		 * and was mistaken for a failed publish more than once.
		 *
		 * kload_space must now read the page pointer. The guest-root
		 * line is expected to stay wrong: it is the host's own
		 * large-page truncation, held in place on purpose, and the
		 * guest's hardware walk resolves the same VA correctly.
		 */
		{
			unsigned long long s = co_vcpu[0].passage_symbol_va
				+ (unsigned long long)lane * 8;
			unsigned long long a = 0, b = 0;

			(void)co_kload_read(manager, s, (unsigned char*)&a, sizeof(a));
			(void)co_kload_read_cr3(manager, co_vcpu[0].guest_cr3, s,
						(unsigned char*)&b, sizeof(b));
			co_debug("vcpu %d: slot 0x%llx -- kload_space reads 0x%llx, "
				 "guest root reads 0x%llx, page is 0x%llx",
				 lane, s, a, b, (unsigned long long)(size_t)pp);
		}

		iterations                       = ~0ULL >> 1;
		out->iterations                  = iterations;
		co_debug("vcpu %d: entering the kernel at 0x%llx", lane,
			 ap_start_rip[lane]);
	}

	/*
	 * Nothing loads CR3 until the addresses this processor cannot run
	 * without have been resolved in the space it is about to enter. Which
	 * space that is decides which walk answers: co_preflight_guest reads
	 * the tables inside the passage page, which are the right ones only
	 * when the vCPU is using them. Joined to the guest, the question is
	 * whether the graft made these pages visible from the kernel's root,
	 * so the walk has to start there.
	 */
	if (join_guest) {
		static const struct { const char* what; int page; } needed[] = {
			{ "switch code",  0 },
			{ "vector stubs", 8 + CO_PP_STUBS_PAGE },
			{ "IST stack",    8 + CO_PP_ISTSTACK_PAGE },
			{ "TSS",          8 + CO_PP_TSS_PAGE },
		};
		/*
		 * Through the root this processor will actually run on. For a
		 * secondary that is the kernel's table, not kload_space's --
		 * walking the wrong one answers a question nobody asked and
		 * passes while the processor is about to fault.
		 */
		/*
		 * A PAGE FRAME NUMBER, not the CR3 value.
		 *
		 * co_arch_guest_lookup_root() takes a pfn and maps it to read
		 * the top-level table; every other caller shifts a physical
		 * address down first. Handing it the raw CR3 asks it to map a
		 * physical address four thousand times higher than the one
		 * meant -- past the end of RAM -- and then to read whatever
		 * that lands on as a PML4 and follow the entries out of it.
		 * The check written to turn a bad secondary into a clean
		 * refusal was itself resetting the machine, before a single
		 * crossing had happened.
		 */
		unsigned long long root_pa = out->waited_for_start
			? pp->linuxvm_state.cr3
			: (unsigned long long)co_arch_guest_space_root(co_kload_space());
		co_pfn_t root = (co_pfn_t)((root_pa & CO_ARCH_PAGE_MASK)
					   >> CO_ARCH_PAGE_SHIFT);
		int i;

		for (i = 0; i < (int)(sizeof(needed) / sizeof(needed[0])); i++) {
			unsigned long long va = (unsigned long long)(size_t)pp
				+ (unsigned long long)needed[i].page * CO_ARCH_PAGE_SIZE;
			co_pa_t got = 0;
			int level = -1;

			if (!CO_OK(co_arch_guest_lookup_root(manager, root, va, &got, &level)) ||
			    got != (co_os_virt_to_phys((void*)(size_t)va) & CO_ARCH_PAGE_MASK)) {
				co_debug_error("vcpu %d: %s at 0x%llx is not reachable in "
					       "the guest's space (absent at level %d) -- "
					       "refusing to enter", lane, needed[i].what,
					       va, level);
				out->preflight_failed = PTRUE;
				out->preflight_va     = va;
				out->preflight_level  = level;
				if (pp != ap_passage[lane]) co_os_free_exec_pages(pp, pages);
				co_vcpu_release(vcpu);
				return CO_RC(ERROR);
			}
		}
	} else if (!co_preflight_guest(pp, setup.passage_va, &setup)) {
		out->preflight_failed = setup.preflight_failed;
		out->preflight_level  = setup.preflight_level;
		out->preflight_va     = setup.preflight_va;
		co_os_free_exec_pages(pp, pages);
		co_vcpu_release(vcpu);
		return CO_RC(ERROR);
	}

	/*
	 * A real secondary runs the REAL monitor loop, not this test lane.
	 *
	 * This is the whole point of the split. The lane below is a measurement
	 * rig -- it counts crossings and checks MSR sentinels -- and it was
	 * never a guest processor. Everything a processor actually needs was
	 * being added to it one operation at a time as production found the
	 * gaps, and the things it still lacked were the ones nothing announces:
	 * extable fixups (CPU 1 died on the rdmsr in perf_msr_probe), the WARN
	 * step-over, INT3 windows from text_poke_bp, tick injection, START_VCPU,
	 * and any bound on how long it may hold the ioctl thread.
	 *
	 * So a secondary that has been given its initial state goes straight
	 * into co_arch_vcpu_run() with the same control struct the boot
	 * processor uses, and returns through the same reporting below.
	 */
	if (out->waited_for_start) {
		co_arch_boot_result_t* ap_result = &ap_result_slot[lane];
		co_rc_t vrc;

		co_memset(ap_result, 0, sizeof(*ap_result));

		out->host_cpu = vcpu->host_cpu;
		out->msr_ok   = PTRUE;

		vrc = co_arch_vcpu_run(manager, vcpu, &vcpu_ctl, ap_result);

		out->completed     = ap_result->switches;
		out->interrupts    = ap_result->interrupts;
		out->faulted       = ap_result->faulted;
		out->vector        = ap_result->vector;
		out->error_code    = ap_result->error_code;
		out->fault_rip     = ap_result->fault_rip;
		out->unforwardable = ap_result->unforwardable;
		out->aborted       = ap_result->hit_deadline;
		out->succeeded     = (!ap_result->faulted && !ap_result->unforwardable)
			? PTRUE : PFALSE;

		co_vcpu_release(vcpu);
		return vrc;
	}

	fn         = (co_switch_full_fn)(void*)pp->code;
	resume_rip = (unsigned long long)(size_t)pp->code
		+ (unsigned long)(&co_guest_resume - &co_switch_full);
	ist_top    = (unsigned long long)(size_t)&pp->host_temp
		+ (CO_PP_ISTSTACK_PAGE + 1) * CO_ARCH_PAGE_SIZE;

	out->host_cpu = vcpu->host_cpu;
	out->msr_ok   = PTRUE;

	for (done = 0; done < iterations; ) {
		if (vcpu_abort_all) {
			out->aborted = PTRUE;
			break;
		}

		/*
		 * A vCPU that moved is not a broken vCPU.
		 *
		 * This was --test-smp's validity check. The spike measured
		 * whether per-crossing state is genuinely per-passage-page, and
		 * a lane that migrated mid-experiment was an invalid data point,
		 * so it stopped and said so. It was then carried into the
		 * production lane whole, where it means something entirely
		 * different: an ordinary Windows scheduling decision kills a
		 * guest processor.
		 *
		 * It is not needed, and the switch is why. Every register the
		 * crossing restores is read from the live processor on the way
		 * out -- sgdt/sidt/sldt/str, the eleven-MSR rdmsr block, TSC_AUX,
		 * xgetbv, CR0/CR2/CR3/CR4/CR8, the six selectors, DR0-3/6/7 (see
		 * co_switch_full above). Nothing is captured once:
		 * co_arch_save_state() at page-build time is overwritten by the
		 * first crossing before it is ever restored from. So a thread
		 * that moved between crossings simply captures the new core's
		 * state on its next exit, and restores that core's state on its
		 * next return.
		 *
		 * And a thread cannot move DURING a crossing: real IF is clear
		 * from before the entry until after co_host_verify, and while
		 * the guest runs the only route back to Windows is this vCPU's
		 * own IDT stub, which world-switches back with the host state
		 * that was saved microseconds earlier on this same core.
		 *
		 * host_cpu is still recorded -- in the vCPU as well as the
		 * report, because co_arch_vcpu_core_taken() scans the vCPU copy
		 * to place later processors. Left at its claim-time value it
		 * would send them away from a core this one no longer occupies.
		 */
		vcpu->host_cpu = co_os_current_cpu();
		out->host_cpu  = vcpu->host_cpu;

		/*
		 * The host's clocks, stamped fresh before every entry -- the
		 * same pair the boot loop stamps at its own entry. params[49]
		 * is the monotonic clock this vCPU's cotimer measures elapsed
		 * time against; left unstamped, a secondary's clock reads as
		 * time standing still and its tick device delivers nothing.
		 */
		pp->params[49] = co_os_monotonic_100ns();
		pp->params[50] = co_os_get_time();

		pp->params[4] = 0;	/* faulted */

		/* Crossing is uninterruptible: real IF clear across it. */
		asm volatile("pushfq; popq %0; cli"
			     : "=r"(host_flags) : : "memory", "cc");
		co_host_snapshot(&host_was);
		fn(&pp->host_state, &pp->linuxvm_state, NULL, 0);

		{
			co_host_field_t bad =
				co_host_verify(&host_was, &out->msr_want,
					       &out->msr_got);

			if (bad != CO_HOST_FIELD_NONE) {
				/*
				 * Report and repair the benign four the way
				 * the boot loop does; anything else means the
				 * other lane is a suspect and the run must
				 * stop with the evidence intact.
				 */
				co_host_repair(&host_was, bad);
				asm volatile("pushq %0; popfq" : : "r"(host_flags)
					     : "memory", "cc");
				if (bad != CO_HOST_FIELD_CR8 &&
				    bad != CO_HOST_FIELD_PAT &&
				    bad != CO_HOST_FIELD_MTRR_DEF &&
				    bad != CO_HOST_FIELD_DR7) {
					out->msr_ok  = PFALSE;
					out->msr_bad = (unsigned long)bad;
					co_debug_error("smp lane %d: host state "
						       "field %d damaged after %lld "
						       "crossings: 0x%llx -> 0x%llx",
						       lane, (int)bad, done,
						       out->msr_want, out->msr_got);
					break;
				}
			} else {
				asm volatile("pushq %0; popfq" : : "r"(host_flags)
					     : "memory", "cc");
			}
		}

		if (!pp->params[4]) {
			done++;

			/*
			 * A voluntary crossing, and for this lane that is all
			 * it is: one more turn of the trivial test guest's own
			 * loop.
			 *
			 * The operation handling that used to live here is
			 * gone, and its absence is the point. A real secondary
			 * never reaches this loop any more -- it returned into
			 * co_arch_vcpu_run() above -- so the operations that
			 * were bolted on here one at a time (BLOCK_IO,
			 * BLOCK_PROBE, KICK_VCPU, IDLE, TERMINATE, YIELD) are
			 * answered by the real monitor loop instead, along with
			 * the ones that were never added: START_VCPU, the
			 * console ring, tick injection, and the whole of the
			 * frame surgery.
			 *
			 * What is left is a measurement rig. It counts
			 * crossings and checks that one lane's MSRs never
			 * appear in another's, which is what --test-smp exists
			 * to prove and is not something a guest processor does.
			 */
			continue;
		}

		out->vector     = pp->params[16];
		out->error_code = pp->params[17];
		out->fault_rip  = pp->params[5];

		if (out->vector < 32) {
			out->faulted = PTRUE;
			co_debug_error("smp lane %d: guest exception vector %lld "
				       "at rip 0x%llx after %lld crossings",
				       lane, out->vector, out->fault_rip, done);
			break;
		}

		{
			unsigned v = (unsigned)(out->vector & 0xff);

			if (!vcpu->seen_vector[v]) {
				vcpu->seen_vector[v] = 1;
				co_debug("smp lane %d: replaying host vector "
					 "0x%x, after %lld crossings",
					 lane, v, done);
			}
		}

		if (!co_arch_forward_host_interrupt(pp->host_state.idt.table,
						    out->vector)) {
			out->unforwardable = PTRUE;
			break;
		}
		out->interrupts++;

		/*
		 * Back in exactly where it was: co_guest_resume replaces the
		 * scratch rsp with the frame the stub recorded. ist_top-0x200
		 * keeps the switch's own CS/RIP pushes clear of that frame.
		 */
		pp->linuxvm_state.return_rip = resume_rip;
		pp->linuxvm_state.rsp        = ist_top - 0x200;
	}

	out->completed = done;
	out->counter   = pp->params[6];
	out->reg_accum = pp->params[7];

	/*
	 * The guest never writes its segment-base MSRs, so the values the
	 * switch saved back on the final exit must be the sentinels planted
	 * before the first entry. Anything else is cross-lane bleed.
	 */
	if (out->msr_ok) {
		struct { unsigned long msr; unsigned long long want, got; } chk[4] = {
			{ CO_MSR_IA32_FS_BASE,        co_smp_sentinel(lane, 0),
			  pp->linuxvm_state.fs_base },
			{ CO_MSR_IA32_GS_BASE,        co_smp_sentinel(lane, 1),
			  pp->linuxvm_state.gs_base },
			{ CO_MSR_IA32_KERNEL_GS_BASE, co_smp_sentinel(lane, 2),
			  pp->linuxvm_state.kernel_gs_base },
			{ CO_MSR_IA32_LSTAR,          co_smp_sentinel(lane, 3),
			  pp->linuxvm_state.lstar },
		};
		int i;

		for (i = 0; i < 4; i++) {
			if (chk[i].got != chk[i].want) {
				out->msr_ok   = PFALSE;
				out->msr_bad  = chk[i].msr;
				out->msr_want = chk[i].want;
				out->msr_got  = chk[i].got;
				co_debug_error("smp lane %d: guest MSR 0x%lx "
					       "sentinel 0x%llx came back 0x%llx",
					       lane, chk[i].msr,
					       chk[i].want, chk[i].got);
				break;
			}
		}
	}

	out->succeeded = (out->completed == out->iterations &&
			  out->counter   == (unsigned long long)out->completed &&
			  out->reg_accum == (unsigned long long)out->completed &&
			  out->msr_ok && !out->faulted && !out->migrated &&
			  !out->unforwardable && !out->aborted) ? PTRUE : PFALSE;

	if (pp != ap_passage[lane])
		co_os_free_exec_pages(pp, pages);
	co_vcpu_release(vcpu);

	return CO_RC(OK);
}

/*
 * Everything that happens once, before any processor executes.
 *
 * Split out of co_arch_boot_loaded() so that what remains -- the crossing loop
 * -- can be run by EVERY processor rather than only by the one that booted.
 * The dividing line is the one the control struct describes: an address used
 * once to get a guest onto the machine belongs here; policy the loop consults
 * on every iteration belongs in co_vcpu_ctl_t and travels to the secondaries.
 *
 * Nothing is freed on the failure paths any more. Every allocation is recorded
 * in ctx as soon as it exists and the single cleanup in co_arch_boot_loaded()
 * gives it back, which is what the two fall-through labels were doing by hand.
 */
static co_rc_t co_arch_boot_prepare(co_manager_t* manager,
				    co_arch_guest_space_t* space,
				    co_arch_boot_t* in, co_arch_boot_result_t* out,
				    co_boot_ctx_t* ctx)
{
	co_arch_passage_page_t* pp;
	co_arch_switch_test_t setup = {0, };
	struct co_console_ring* ring;
	/*
	 * The guest's boot stack, in a PML4 slot of its own.
	 *
	 * It used to sit at 0xffffffff90000000, which shares the top-level slot
	 * with the kernel image -- fine while the host owned the whole address
	 * space, fatal once the guest is switched into the kernel's own tables:
	 * that slot then belongs to level3_kernel_pgt, which maps the image and
	 * nothing else, and the stack vanished under the first push.
	 *
	 * 0xffff908000000000 is in the 118 TB the host does not use, measured by
	 * the PML4 sweep, and in a slot nothing else claims.
	 */
	unsigned long long ring_va, stack_va = 0xffff908000000000ULL;
	co_pfn_t stack_pfn = 0;
	int pages = sizeof(co_arch_passage_page_t) / CO_ARCH_PAGE_SIZE;
	int page;
	co_rc_t rc;

	co_memset(out, 0, sizeof(*out));
	out->supported = PTRUE;

	if (space == NULL || !in->entry_va || !in->initial_code_va || !in->start_kernel_va)
		return CO_RC(ERROR);

	/*
	 * Refuse if anything else has VMX claimed.
	 *
	 * The switch clears CR4.PGE on every crossing. Under another hypervisor
	 * the machine is in VMX non-root mode, where CR4 writes are the other
	 * hypervisor's to interpret -- coLinux's own history records this as
	 * STOP 0x7F, UNEXPECTED_KERNEL_MODE_TRAP, which is a double fault and
	 * takes the host with it. Aborting the guest is the only sane answer, and
	 * it is what the i386 port learned to do.
	 */
	if (co_get_cr4() & CO_ARCH_X86_CR4_VMXE) {
		co_debug_error("CR4.VMXE is set: something else has VMX claimed. "
			       "Clearing CR4.PGE under another hypervisor is a "
			       "double fault, so refusing to run a guest.");
		out->vmx_present = PTRUE;
		return CO_RC(ERROR);
	}

	pp = co_setup_guest_page(&setup,
		(unsigned long)(&co_switch_guest_entry - &co_switch_full));
	if (pp == NULL)
		return CO_RC(ERROR);

	/* Recorded the instant it exists, so every exit below can be a plain
	 * return and the single cleanup in the caller still gives it back. */
	ctx->pp = pp;

	for (page = 0; page < pages; page++) {
		unsigned char* q = (unsigned char*)pp + page * CO_ARCH_PAGE_SIZE;

		rc = co_arch_guest_map(manager, space, (unsigned long long)(size_t)q,
				       co_os_virt_to_phys(q), _KERNPG_TABLE);
		if (!CO_OK(rc))
			return rc;
	}

	rc = co_os_get_page(manager, &stack_pfn);
	if (!CO_OK(rc))
		return rc;
	ctx->stack_pfn = stack_pfn;
	rc = co_arch_guest_map(manager, space, stack_va,
			       ((co_pa_t)stack_pfn) << CO_ARCH_PAGE_SHIFT, _KERNPG_TABLE);
	if (!CO_OK(rc))
		return rc;

	/*
	 * The secondaries' passage pages, here and not when they start. See
	 * ap_passage. Not fatal if one cannot be made: the guest simply cannot
	 * start that processor, and the lane refuses rather than entering
	 * something unreachable.
	 */
	{
		int v;

		/*
		 * Cleared first, every run.
		 *
		 * ap_passage[] is static, so without this it keeps whatever a
		 * previous run left in it -- a pointer to memory that run
		 * freed. The secondary's own guard is "was a page made for me
		 * at boot", and a stale non-NULL pointer answers yes, so it
		 * builds its passage page on top of somebody else's freed
		 * memory instead of refusing. That is invisible until the
		 * machine resets.
		 */
		for (v = 0; v < CO_MAX_VCPUS; v++) {
			ap_passage[v] = NULL;
			/*
			 * And the request with it. The reasoning above applies
			 * word for word to the start block: left set from a
			 * previous run, a secondary polling early in this one
			 * finds a request nobody made and enters at the last
			 * run's rip, on its stack, through its CR3 -- physical
			 * addresses freed at that run's KLOAD_END and since
			 * handed to something else.
			 */
			ap_start_pending[v] = 0;
			ap_start_rip[v]     = 0;
			ap_start_rsp[v]     = 0;
			ap_start_cr3[v]     = 0;
			ap_start_gs[v]      = 0;
		}

		/*
		 * And the boot processor's extended-state capture, for exactly
		 * the reason given above and in the same place.
		 *
		 * vcpu0_cr4 and vcpu0_xcr0 are file-scope statics with no
		 * lifetime of their own, so a second run in one driver load
		 * starts its secondary on the PREVIOUS run's CR4 and XCR0. CR4
		 * is the dangerous half: bit 18 is OSXSAVE, and the switch only
		 * restores XCR0 at all when that bit is set (the bt $18 guard in
		 * co_switch_full), so a stale CR4 decides whether the extended
		 * state travels. This is the same class of bug the comment on
		 * ap_passage[] above was written about -- "a stale non-NULL
		 * value answers yes ... invisible until the machine resets" --
		 * reintroduced three screens below it.
		 */
		vcpu0_cr4  = 0;
		vcpu0_xcr0 = 0;

		for (v = 1; v < CO_MAX_VCPUS; v++) {
			co_arch_switch_test_t aps = {0, };
			co_arch_passage_page_t* app;
			int q;

			app = co_setup_guest_page(&aps,
				(unsigned long)(&co_boot_shim - &co_switch_full));
			if (app == NULL) {
				/*
				 * Say so. Fifteen executable pages per secondary,
				 * asked for after the guest's RAM is already
				 * committed, is a request that can fail -- and a
				 * silent break here is what let a stale pointer
				 * stand in for a page that was never made.
				 */
				co_debug_error("boot: no passage page for vcpu %d "
					       "(%d exec pages unavailable) -- that "
					       "processor cannot be started", v, pages);
				break;
			}

			for (q = 0; q < pages; q++) {
				unsigned char* w = (unsigned char*)app
					+ q * CO_ARCH_PAGE_SIZE;

				if (!CO_OK(co_arch_guest_map(manager, space,
							     (unsigned long long)(size_t)w,
							     co_os_virt_to_phys(w),
							     _KERNPG_TABLE))) {
					co_os_free_exec_pages(app, pages);
					app = NULL;
					break;
				}
			}

			ap_passage[v] = app;
			if (app)
				co_debug("boot: vcpu %d passage page 0x%llx, mapped "
					 "before the graft", v,
					 (unsigned long long)(size_t)app);
		}
	}

	ring = (struct co_console_ring*)((unsigned char*)&pp->host_temp
					 + CO_PP_CONSOLE_PAGE * CO_ARCH_PAGE_SIZE);
	ring_va = (unsigned long long)(size_t)ring;
	co_memset(ring, 0, CO_ARCH_PAGE_SIZE);
	ring->capacity = CO_ARCH_PAGE_SIZE - sizeof(*ring);
	/*
	 * The early console stays vCPU 0's, and that is not an oversight.
	 *
	 * It lives in the boot processor's passage page and the guest holds one
	 * global pointing at it, so it is load-time setup and result harvesting
	 * rather than loop work -- which is why it does not appear in
	 * co_vcpu_ctl_t. A secondary never writes it: earlycon is gone long
	 * before smp_init() starts one, and printk serialises the writers that
	 * remain behind the console lock regardless.
	 */
	ctx->ring = ring;

	if (!CO_OK(co_write_guest_u64(manager, space, in->ring_symbol_va, ring_va)) ||
	    !CO_OK(co_write_guest_u64(manager, space, in->early_console_va,
				      in->colinux_console_va)) ||
	    !CO_OK(co_write_guest_u64(manager, space, in->initial_code_va,
				      in->start_kernel_va)) ||
	    (in->guest_flag_va &&
	     !CO_OK(co_write_guest_u64(manager, space, in->guest_flag_va, 1))) ||
	    /*
	     * Where the guest finds the world switch, so it can call it: the
	     * passage page has the same address in both spaces, and the guest's
	     * cooperative yield (co_idle_processor) calls pp->code directly.
	     */
	    (in->passage_symbol_va &&
	     !CO_OK(co_write_guest_u64(manager, space, in->passage_symbol_va,
				       (unsigned long long)(size_t)pp)))) {
		co_debug_error("could not write the boot globals");
		return CO_RC(ERROR);
	}

	pp->linuxvm_state.cr3 = co_arch_guest_space_root(space);

	/*
	 * Switch into the kernel's own address space, if it gave us one.
	 *
	 * The space the host built got the guest as far as running; it cannot
	 * get it further, because the kernel walks and edits the tables it was
	 * linked with and finds the host's arrangement instead. Adopting them
	 * relocates those tables to where the image actually is and grafts the
	 * host's mappings -- the direct map, and the passage page this very code
	 * is executing from -- into the top level.
	 */
	if (in->kernel_table_count > 0) {
		unsigned long long kcr3 = 0;

		rc = co_kload_adopt_kernel_tables(manager, in->kernel_tables,
						  in->kernel_table_count, &kcr3);
		if (!CO_OK(rc)) {
			co_debug_error("boot: could not adopt the kernel's page tables");
			return rc;
		}
		pp->linuxvm_state.cr3 = kcr3;
	}
	pp->params[19]        = (unsigned long long)(size_t)pp->code;

	/* Enter through the boot shim with real IF clear and virtual IF enabled. */
	pp->params[20] = in->entry_va;
	pp->params[29] = in->step ? 1 : 0;
	pp->params[48] = 0;	/* virtual ticks banked while the host slept at IDLE */
	/*
	 * Once, before the first entry. The loop no longer clears this every
	 * time round -- see the comment there -- so it has to start empty.
	 */
	pp->operation  = CO_OPERATION_EMPTY;
	pp->linuxvm_state.return_rip = (unsigned long long)(size_t)pp->code
		+ (unsigned long)(&co_boot_shim - &co_switch_full);
	pp->linuxvm_state.rsp = stack_va + CO_ARCH_PAGE_SIZE - 0x40;

	out->guest_cr3      = pp->linuxvm_state.cr3;
	out->entry_va       = in->entry_va;
	out->console_ring_va = ring_va;
	out->tables         = co_arch_guest_space_tables(space);

	if (!CO_OK(co_preflight_space(manager, space, "entry", in->entry_va, 0, &setup)) ||
	    !CO_OK(co_preflight_space(manager, space, "stack", stack_va,
				      ((co_pa_t)stack_pfn) << CO_ARCH_PAGE_SHIFT, &setup)) ||
	    !CO_OK(co_preflight_space(manager, space, "start_kernel",
				      in->start_kernel_va, 0, &setup)) ||
	    !CO_OK(co_preflight_space(manager, space, "passage page",
				      (unsigned long long)(size_t)pp,
				      co_os_virt_to_phys(pp), &setup))) {
		out->preflight_failed = PTRUE;
		out->preflight_va     = setup.preflight_va;
		out->preflight_level  = setup.preflight_level;
		return CO_RC(ERROR);
	}
	out->preflight_checked = setup.preflight_checked;

	/*
	 * Say where we are before every step that could be the last one.
	 *
	 * The previous attempt logged nothing between co_kload_build_ram and the
	 * reset, so all it established was "somewhere after the RAM was built" --
	 * which is most of the work. Each of these is one UDP packet already on the
	 * host by the time the instruction after it runs, so whatever the machine
	 * does next, the log says what it was about to do.
	 */
	/*
	 * Which driver build this run came from.
	 *
	 * The load-time stamp cannot be relied on: the driver is loaded before
	 * the debug daemon raises the facility levels, so that record is
	 * discarded at source. This one is emitted with the levels already up,
	 * on every run, so a log can always be tied to the code that produced
	 * it. A stale resident image reading a freshly built daemon's ioctl
	 * struct at the wrong offsets is not a failure that announces itself.
	 */
	/*
	 * The guest's exception table, before it runs.
	 *
	 * Not fatal if it is missing: a run without it behaves exactly as every
	 * run did before, stopping at the first fault the kernel meant to
	 * recover from. Saying so is the point -- a boot that dies at
	 * __switch_to with no note here would send the next reader after the
	 * segment load rather than after the table.
	 */
	if (in->ex_table_start && in->ex_table_stop) {
		if (!CO_OK(co_arch_extable_load(manager, in->ex_table_start,
						in->ex_table_stop)))
			co_debug_error("boot: no exception table -- recoverable "
				       "faults will stop the run");
	} else {
		co_debug("boot: no exception table given");
	}

	co_debug("boot: driver built " __DATE__ " " __TIME__);
	co_debug("boot: passage page 0x%llx, guest cr3 0x%llx, %ld tables",
		 (unsigned long long)(size_t)pp, out->guest_cr3, out->tables);
	co_debug("boot: entry 0x%llx, stack 0x%llx, ring 0x%llx",
		 in->entry_va, stack_va, ring_va);
	co_debug("boot: globals poked -- initial_code -> 0x%llx, guest flag at 0x%llx",
		 in->start_kernel_va, in->guest_flag_va);
	co_debug("boot: preflight passed %d addresses; entering now, step=%d",
		 out->preflight_checked, in->step);

	return CO_RC(OK);
}

/*
 * The monitor loop -- and it is now EVERY processor's, not the boot one's.
 *
 * The guest runs until something interrupts it. External vectors (32 and
 * above) are the host's -- forward them to Windows and put the guest back
 * exactly where it was. Anything below 32 is an exception the guest itself
 * took, which is news, so stop and report it.
 *
 * Bounded, because an unbounded loop here is the same bug as an unbounded
 * guest: this runs inside a driver ioctl and has to give the thread back.
 *
 * This used to be welded into co_arch_boot_loaded() while a secondary ran the
 * S0 test lane instead, and operations were bolted onto that lane one at a time
 * as each one broke something in production. What the lane never gained is the
 * part that matters most: the frame surgery. A vector below 32 stopped a
 * secondary dead, so the extable fixup the guest was relying on never ran --
 * measured as CPU 1 dying on the rdmsr in perf_msr_probe, and as a silent
 * mid-task freeze that the guest only ever reported as an RCU stall. Every
 * text_poke_bp() INT3 window is the same shape. It also never injected a tick,
 * so a userspace task on a secondary that made no syscalls could not be
 * preempted at all, and never handled START_VCPU, so onlining a third processor
 * from a task that happened to be on the second one killed it with "unhandled
 * guest operation 19".
 *
 * There is one loop because there was never a structural reason for two: every
 * register the crossing needs is saved and restored per crossing, per passage
 * page, and a vCPU is a vCPU.
 */
static co_rc_t co_arch_vcpu_run(co_manager_t* manager, co_vcpu_t* vcpu,
				const co_vcpu_ctl_t* ctl, co_arch_boot_result_t* out)
{
	co_arch_passage_page_t* pp = vcpu->pp;
	co_switch_full_fn fn = (co_switch_full_fn)(void*)pp->code;
	{
		unsigned long long resume_rip = (unsigned long long)(size_t)pp->code
			+ (unsigned long)(&co_guest_resume - &co_switch_full);
		unsigned long long ist_top = (unsigned long long)(size_t)&pp->host_temp
			+ (CO_PP_ISTSTACK_PAGE + 1) * CO_ARCH_PAGE_SIZE;
		co_host_snapshot_t host_was;
		/*
		 * The stub's frame, bounced through the guest's page tables:
		 * fifteen GPRs, vector, error code, then the CPU's five-word
		 * iretq frame -- 0xb0 bytes. Read fresh every fault crossing,
		 * edited here, written back by whichever resume path changed
		 * it. See the comment at the read for why a direct pointer
		 * dereference is a host bugcheck.
		 */
		unsigned long long frame_buf[0xb0 / 8];
		unsigned long long* frame;
		/*
		 * The budget counts guest crossings, not crossings.
		 *
		 * With the guest free-running on real interrupts, the great
		 * majority of returns from the switch are Windows' own clock
		 * arriving through the guest's IDT -- 399207 of the last run's
		 * 400000. Charging those to the guest's budget meant the limit
		 * measured how long the host's timer had been ticking, so a
		 * run "of 400000 switches" was seven minutes of wall clock in
		 * which the guest did almost nothing, and a guest that booted
		 * and idled could not finish early because the ticks kept
		 * spending its allowance. That is not a bound on the guest at
		 * all, and it is the opposite of running at native speed.
		 *
		 * So a replayed hardware interrupt is free: it is the host's
		 * business, it happens at whatever rate Windows wants, and the
		 * guest is not making progress through it. What the budget
		 * bounds is what the guest does -- voluntary yields, faults,
		 * single steps.
		 *
		 * The wall-clock deadline is the real backstop, and the honest
		 * one. This runs inside a driver ioctl and must give the
		 * thread back; the thing that matters is elapsed time, which
		 * is now measured directly rather than inferred from a count
		 * whose cost per unit was never constant.
		 */
		/*
		 * A guest nobody is talking to gets the bring-up bound; one
		 * with a terminal attached gets long enough to be used, since
		 * a person at a prompt is the slowest thing in the system.
		 *
		 * Decided by the caller now and carried in the control struct,
		 * because it is policy the loop consults rather than something
		 * the loop can work out: a secondary has to observe the same
		 * bound as the boot processor, and it never had one at all --
		 * its lane ran to LLONG_MAX iterations, so a secondary that
		 * missed the abort broadcast stayed inside the driver for good.
		 */
		unsigned long deadline_secs = ctl->deadline_secs;
		/* Zero seconds means no deadline; see switch.h. */
		unsigned long long deadline = deadline_secs
			? co_os_monotonic_100ns() + deadline_secs * 10000000ULL
			: 0;
		unsigned long guest_crossings = 0;
		unsigned long idle_run = 0;
		/*
		 * When a cooperative tick was last delivered, in host monotonic
		 * 100 ns units. Zero until the first interrupt crossing, which
		 * arms it rather than firing -- there is no sensible "last" on
		 * the first pass and injecting against an unset baseline would
		 * deliver a tick immediately at every boot.
		 */
		unsigned long long tick_last = 0;
		/*
		 * Heartbeat: one debug record per second of wall time, streamed
		 * over UDP by the debug daemon as it is produced, so it survives
		 * the host dying. Everything else written tonight -- the boot
		 * daemon's report, the guest's console -- is buffered somewhere
		 * that dies with the machine, which is why five freezes have
		 * produced zero evidence. This is the flight recorder.
		 */
		unsigned long long hb_last = co_os_monotonic_100ns();
		unsigned long hb_iter = 0, hb_vol = 0, hb_idle = 0, hb_blk = 0;
		int i;

		for (i = 0; ; i++) {
			unsigned long long batch;
			unsigned long long host_flags;

			/*
			 * The driver is going away and this thread is inside
			 * it. Stop now, before the next co_debug writes into a
			 * debug system that is being torn down.
			 */
			if (vcpu_abort_all) {
				out->hit_deadline = PTRUE;
				break;
			}

			/*
			 * Zero is no limit, for the same reason the console
			 * deadline is: an interactive guest must not be
			 * stopped out from under whoever is using it.
			 */
			if (ctl->max_switches &&
			    guest_crossings >= ctl->max_switches) {
				out->hit_limit = PTRUE;
				break;
			}

			pp->params[4] = 0;		/* faulted */

			/*
			 * pp->operation is deliberately NOT cleared here.
			 *
			 * The guest writes it and then calls the switch, and
			 * those are two instructions with a gap between them.
			 * Now that the guest runs with real interrupts on, a
			 * hardware interrupt can land in that gap: the stub
			 * crosses back, this loop forwards the interrupt and
			 * resumes the guest, and the guest then executes its
			 * call -- with the operation this loop had just wiped.
			 * The crossing arrives as CO_OPERATION_EMPTY and the
			 * run stops with 'unhandled operation 0', naming
			 * nothing.
			 *
			 * Each voluntary branch below clears the field once it
			 * has read it, which is the only point at which the
			 * value is known to have been consumed.
			 */

			/*
			 * How many instructions the guest may step through on
			 * its own before it has to come back.
			 *
			 * This is the whole speed of the thing. At one world
			 * switch per instruction it ran at 280,000
			 * instructions a second; the stub handles a debug trap
			 * in about twenty instructions without leaving the
			 * guest, so a batch of this size turns one crossing
			 * into thousands of executed instructions.
			 *
			 * Sized against the thing it is protecting: this is
			 * how long a guest that cannot be interrupted may hold
			 * the processor. A few thousand stepped instructions
			 * is well under a millisecond, which is far less than
			 * the host would lose to a single page fault.
			 */
			batch = (ctl->step && ctl->batch > 0)
				? (unsigned long long)ctl->batch : 0;
			pp->params[30] = batch;

			/*
			 * A progress mark every 64 steps, and the address the
			 * guest is about to resume at.
			 *
			 * This is the only diagnostic that survives the guest
			 * running away. Everything else -- the trace ring, the
			 * fault record, the step count -- travels home in the
			 * ioctl return, and there is no return: the guest keeps
			 * executing inside this loop's world switch, the thread
			 * never comes back, and the machine has to be power
			 * cycled. These records leave the box while it is still
			 * running, drained by a separate process on another
			 * core, so the last one written says where the guest was
			 * when control was lost, to within 64 instructions.
			 *
			 * Every instruction would be a packet per instruction
			 * and would change what is being measured; every 64 is
			 * about 1500 packets over a full run.
			 */
			/* nothing here: the guest's address is only known after
			 * the switch returns, and is logged there */

			/*
			 * Isolate the crossing from host interrupt delivery. The guest
			 * returns voluntarily with the saved host IF still clear, which
			 * lets us verify the switch before any legitimate Windows ISR can
			 * alter per-CPU scratch state such as CR2. After verification the
			 * exact original flags are restored; pending hardware interrupts
			 * then enter through Windows' own IDT before we re-enter the guest.
			 */
			/*
			 * The host's clocks, stamped fresh before every entry.
			 *
			 * params[49] is the monotonic clock in 100 ns units;
			 * the guest's cooperative tick device measures its
			 * elapsed time against it and synthesises that many
			 * ticks (co_colinux_drain_time in the guest tree).
			 * params[50] is wall time for get_wallclock. Stamped
			 * here rather than once, because a stale clock reads
			 * as time standing still -- the exact failure virtual
			 * time exists to end.
			 */
			pp->params[49] = co_os_monotonic_100ns();
			pp->params[50] = co_os_get_time();

			/*
			 * Asked now, not once at the top, because at the top the
			 * answer is always no.
			 *
			 * co_console_get_address() is the address the *guest*
			 * publishes when its console driver initialises, which
			 * happens some way into the boot -- long after this loop
			 * starts. Choosing the deadline from it at loop entry
			 * therefore always chose the headless one, and every
			 * interactive session has been getting the bring-up
			 * bound of CO_BOOT_MAX_SECONDS regardless of the
			 * CO_BOOT_CONSOLE_SECONDS 0 that was supposed to mean
			 * "no deadline". A guest with somebody at a prompt was
			 * killed by a timer two minutes in, which is exactly
			 * what that zero exists to prevent.
			 *
			 * The headless bound stays and stays useful: a bring-up
			 * run with nobody attached still has to stop by itself
			 * and report. What changes is that attaching a console
			 * lifts it, at whatever point the guest gets far enough
			 * to have one.
			 */
			if (deadline && co_console_get_address() != 0) {
				co_debug("boot: console attached after %ld "
					 "switches -- lifting the %ld second "
					 "deadline", out->switches, deadline_secs);
				deadline = 0;
			}

			if (deadline && pp->params[49] >= deadline) {
				out->hit_deadline = PTRUE;
				co_debug("boot: %ld second deadline reached after "
					 "%ld switches (%ld of them the guest's, "
					 "%ld replayed interrupts)",
					 deadline_secs, out->switches,
					 guest_crossings, out->interrupts);
				break;
			}

			asm volatile("pushfq; popq %0; cli"
				     : "=r"(host_flags) : : "memory", "cc");
			co_host_snapshot(&host_was);
			fn(&pp->host_state, &pp->linuxvm_state, NULL, 0);

			out->switches++;

			/*
			 * Everything the stub stepped through without asking.
			 * The counter it decremented is the only record of it,
			 * and it has to be collected before the next crossing
			 * overwrites it.
			 */
			if (batch)
				out->steps += batch - pp->params[30];

			/*
			 * Before anything else, and before any of the state
			 * that just came back is used or logged: did the host
			 * survive intact? A crossing that returns with the
			 * wrong IDTR or the wrong GS base has already killed
			 * the machine, it just has not fallen over yet.
			 */
			{
				co_host_field_t bad =
					co_host_verify(&host_was,
						       &out->host_corrupt_expected,
						       &out->host_corrupt_actual);

				if (bad != CO_HOST_FIELD_NONE) {
					co_host_snapshot_t now;

					/* the first divergence is the record */
					if (!out->host_corrupt_field) {
						out->host_corrupt_field = (int)bad;
						out->host_corrupt_step  = out->steps;
					}

					/*
					 * The whole snapshot, not just the first
					 * field that differed. Reporting one
					 * field by number invites reading the
					 * number wrong -- and a value that looks
					 * like the wrong kind of address for the
					 * register it is attributed to is the
					 * only clue that the numbering and the
					 * name table have drifted apart.
					 */
					co_host_snapshot(&now);
					co_debug("boot: HOST STATE DAMAGED after "
						 "%ld steps: field %d, was 0x%llx, "
						 "now 0x%llx",
						 out->steps, (int)bad,
						 out->host_corrupt_expected,
						 out->host_corrupt_actual);
					/*
					 * Reported first, then repaired: PAT,
					 * CR8 and DR7 go back to the snapshot
					 * so Windows is not left running on
					 * the guest's values while the report
					 * travels home.
					 */
					co_host_repair(&host_was, bad);
					asm volatile("pushq %0; popfq" : : "r"(host_flags)
						     : "memory", "cc");

					/*
					 * Those four are plain values with no
					 * machinery behind them, so a repaired
					 * run is a healthy run and continues --
					 * that is how the culprit gets named
					 * AND the box survives to deliver the
					 * report. Anything else means the
					 * switch itself failed to carry state
					 * it owns, and nothing after that is
					 * trustworthy.
					 *
					 * MTRRdefType belongs in this group and
					 * is the most urgent of them to put back:
					 * with its enable bit clear the host runs
					 * entirely uncached, so leaving it for the
					 * report to explain would mean writing
					 * that report at a thousandth of speed.
					 */
					if (bad == CO_HOST_FIELD_CR8 ||
					    bad == CO_HOST_FIELD_PAT ||
					    bad == CO_HOST_FIELD_MTRR_DEF ||
					    bad == CO_HOST_FIELD_DR7)
						goto host_repaired;

					co_debug("boot:   cpu   %ld -> %ld",
						 host_was.cpu, now.cpu);
					co_debug("boot:   cr3   0x%llx -> 0x%llx",
						 host_was.cr3, now.cr3);
					co_debug("boot:   cr4   0x%llx -> 0x%llx",
						 host_was.cr4, now.cr4);
					co_debug("boot:   gdt   0x%llx -> 0x%llx",
						 host_was.gdt_base, now.gdt_base);
					co_debug("boot:   idt   0x%llx -> 0x%llx",
						 host_was.idt_base, now.idt_base);
					co_debug("boot:   gsbase 0x%llx -> 0x%llx",
						 host_was.gs_base, now.gs_base);
					break;
				}

				asm volatile("pushq %0; popfq" : : "r"(host_flags)
					     : "memory", "cc");
			}
	host_repaired:

			hb_iter++;
			if ((i & 0xff) == 0) {
				unsigned long long hb_now = co_os_monotonic_100ns();

				if (hb_now - hb_last >= 10000000ULL) {
					/*
					 * rx_spins is gone from here because it
					 * is gone from the loop: CONET_PUT rings
					 * the doorbell now, so nothing re-enters
					 * on pending frames and the counter was
					 * reporting a constant zero.
					 */
					co_debug("hb: %lu crossings/s (%lu voluntary,"
						 " %lu idle, %lu blkio), idle_run %lu",
						 hb_iter, hb_vol, hb_idle, hb_blk,
						 idle_run);
					hb_last = hb_now;
					hb_iter = hb_vol = hb_idle = hb_blk = 0;
				}
			}

			if (!pp->params[4]) {
				/*
				 * A voluntary crossing: the guest called the
				 * switch itself, and pp->operation says why.
				 * This is the cooperative protocol -- no fault,
				 * no stub, the guest's own resume point saved
				 * by the switch, so re-entering needs no frame
				 * surgery at all: the same fn() call restores
				 * it exactly where its yield left off.
				 */
				unsigned long long op = pp->operation;

				guest_crossings++;	/* the guest's own doing */
				hb_vol++;

				pp->operation = 0;

				/*
				 * Block I/O, done here and not by the guest.
				 *
				 * The guest hands over a unit, an offset, a
				 * length and a guest physical address, and the
				 * host reads or writes the backing store
				 * straight into that memory -- no bounce
				 * buffer, because guest physical is host
				 * physical and the driver allocated the pages.
				 * Synchronous: the transfer completes before
				 * the guest's own call returns, which is what
				 * lets the guest-side driver be a few dozen
				 * lines with no completion path.
				 *
				 * This runs in host context at PASSIVE_LEVEL
				 * inside the ioctl, which is where file I/O is
				 * legal -- the reason the transfer happens
				 * after the crossing rather than being
				 * attempted from inside the guest.
				 */
				/*
				 * Anything that is not IDLE is the guest doing
				 * work, so the next idle is a pause inside a
				 * busy period rather than the guest having
				 * nothing to do. See the sleep below.
				 */
				if (op != CO_OPERATION_IDLE)
					idle_run = 0;

				if (op == CO_OPERATION_BLOCK_IO) {
					co_rc_t brc;

					/*
					 * params[54] is the guest physical
					 * address of a descriptor list and
					 * params[53] is how many descriptors it
					 * holds -- a whole request in one
					 * crossing rather than one per page.
					 */
					if (ctl->async_cobd && ctl->cobd_io_va) {
						/*
						 * Async (default): queue the
						 * request to a worker thread and
						 * return to the guest at once, so
						 * the single guest CPU is not held
						 * for the whole transfer. The
						 * completion comes back through the
						 * co_colinux_cobd_io ring, reaped on
						 * the guest's tick. params[56] is
						 * now accepted(0)/busy(1); params[57]
						 * carries the blk-mq tag.
						 */
						bool_t ok = co_cobd_async_submit(
							(int)pp->params[51],
							pp->params[52],
							pp->params[54],
							(unsigned int)pp->params[53],
							pp->params[55] ? PTRUE : PFALSE,
							(unsigned int)pp->params[57]);

						pp->params[56] = ok ? 0 : 1;
						hb_blk++;
						out->block_requests++;
						continue;
					}

					/*
					 * Sync fallback (--sync-cobd): perform the
					 * transfer inline. Correct but freezes the
					 * guest for its duration -- defect 3.
					 */
					brc = co_cobd_request_sg(manager,
							      (int)pp->params[51],
							      pp->params[52],
							      pp->params[54],
							      (unsigned int)pp->params[53],
							      pp->params[55] ? PTRUE : PFALSE);

					pp->params[56] = CO_OK(brc) ? 0 : 1;
					hb_blk++;
					out->block_requests++;
					if (!CO_OK(brc))
						out->block_errors++;
					continue;
				}

				/*
				 * How many units there are and how big each
				 * one is. Asked once per unit as the guest's
				 * driver probes, so that the guest's idea of
				 * the geometry comes from the host rather than
				 * from a constant the two could disagree on.
				 */
				if (op == CO_OPERATION_BLOCK_PROBE) {
					int unit = (int)pp->params[51];

					pp->params[52] = co_cobd_present(unit)
						? co_cobd_size(unit) : 0;
					pp->params[56] = 0;
					continue;
				}

				if (op == CO_OPERATION_IDLE) {
					hb_idle++;
					out->idle_yields++;
					if (out->idle_yields == 1) {
						out->reached_idle = PTRUE;
						co_debug("boot: first cooperative "
							 "IDLE yield, after %ld "
							 "switches", out->switches);
					}

					/*
					 * Sleep. This is the whole difference
					 * between cooperative idle and the
					 * freeze that ended every full run.
					 *
					 * Re-entering immediately ping-pongs
					 * the crossing at full speed, and the
					 * crossing runs under cli: the pinned
					 * core spends nearly all of its time
					 * with interrupts masked, Windows
					 * loses its clock on that core, and
					 * the machine freezes within a second
					 * of the guest going idle -- exactly
					 * when the boot finally works.
					 *
					 * coLinux's own monitor sleeps at IDLE
					 * (monitor.c: iteration()) and wakes on
					 * work. There is no message queue yet,
					 * so the wake condition is time: one
					 * host tick, with the host's flags
					 * already restored above, so Windows
					 * owns the core for the duration.
					 *
					 * Each tick slept is banked in
					 * params[48] for the guest's virtual
					 * clock to drain -- until the guest
					 * learns to, the count is simply
					 * unread, and jiffies not advancing is
					 * the next piece of work, not a hazard.
					 */
					/*
					 * How long to sleep, and why the answer
					 * is now one number rather than a ladder.
					 *
					 * Ten milliseconds was chosen when the
					 * only question was whether an idle
					 * guest could stop pinning the core. It
					 * was also the floor on every round trip
					 * the guest makes, because a guest
					 * waiting for a reply is by definition
					 * idle: a ping to 10.0.2.2 -- slirp's
					 * gateway, answered by a process on this
					 * same machine with no network involved
					 * at all -- measured 10 ms, and a
					 * download sat at 1.39 MB/s from a
					 * mirror 5 ms away and 1.64 MB/s from
					 * another continent, which is what a
					 * limit inside the host looks like from
					 * outside.
					 *
					 * That was answered with a ladder: one
					 * millisecond while the guest was busy,
					 * ten after a hundred consecutive idle
					 * yields. The ladder was itself a bug.
					 * This wait is the guest's clock, so
					 * backing off to ten milliseconds was
					 * running its 1000 Hz tick device at
					 * 100 Hz whenever a processor had been
					 * idle a moment -- and the secondary's
					 * flat hundred ran it at 10 Hz for that
					 * processor's whole life.
					 *
					 * So the wait is the tick period, always,
					 * and latency is the doorbell's job
					 * rather than the timeout's. A reply that
					 * arrives now wakes the processor now:
					 * conet RX, block completions, console
					 * input, the GPU daemon and the IPI
					 * doorbell all ring. The timeout only
					 * has to be short enough to keep the
					 * clock honest, and the tick period is
					 * exactly that length by definition.
					 */

					/*
					 * The sleep is now a wait: same
					 * bounded backoff, but the GPU daemon
					 * can cut it short through the
					 * VGPU_WAKE ioctl the moment it
					 * publishes a completion. This is the
					 * completion doorbell -- the fix the
					 * failed experiments pointed at: an
					 * event the daemon signals through
					 * the driver, not a re-entry loop.
					 * Measured before it: every fence a
					 * guest waited on cost a full backoff
					 * tick (~10 ms flat, offscreen EGL,
					 * any resolution) because the
					 * completion is a plain store into
					 * guest RAM that nothing here can
					 * see. The guest reaps at its
					 * idle-boundary drain even when no
					 * tick is due, so re-entering on the
					 * wake is sufficient; the timeout
					 * path is byte-for-byte the old
					 * behaviour, so the anti-freeze
					 * property this sleep exists for is
					 * untouched. The banked tick stays
					 * timeout-only: a wake re-entry took
					 * no time worth accounting.
					 */
					/*
					 * Whichever comes first: a doorbell, or
					 * this guest's next tick.
					 *
					 * The doorbells are what make a wake
					 * IMMEDIATE -- the IPI, START_VCPU, the
					 * GPU daemon, block completions, conet
					 * RX, console input, and every abort
					 * path through co_arch_boot_abort(). A
					 * completion no longer waits out a
					 * timeout to be noticed.
					 *
					 * But the timeout cannot be removed,
					 * because it is not only a backstop: it
					 * is the guest's CLOCK. co_colinux_take_
					 * ticks() turns elapsed host time into
					 * tick-handler calls, and it only runs
					 * when this thread wakes and re-enters.
					 * Waiting indefinitely stops the clock
					 * on this processor -- every msleep(),
					 * schedule_timeout() and timer_list
					 * stops expiring -- which measured as
					 * cobd0 mounting at 13.173s instead of
					 * 0.348s and systemd sitting in
					 * 90-second start-job timeouts.
					 *
					 * So the bound is the tick period, and
					 * it is the tick period rather than a
					 * ladder: the old "idle_run > 100 ? 10 :
					 * 1" was running the guest's clock at
					 * 100 Hz whenever a processor had been
					 * idle a while, and the secondary's flat
					 * 100 ms ran it at 10 Hz permanently.
					 *
					 * The deadline needs no arithmetic here.
					 * This wakes every tick and the loop
					 * re-checks the deadline at the top of
					 * every iteration, so a deadline is
					 * observed within one tick of expiring.
					 */
					idle_run++;
					if (!co_os_idle_wait(vcpu->index,
							     CO_GUEST_TICK_MS))
						pp->params[48] += 1;

					/*
					 * Idle forever is correct cooperative
					 * behaviour, but this loop still runs
					 * inside an ioctl and has to give the
					 * thread back while bring-up needs a
					 * result to read. The bound is now
					 * expressed as seconds of idle converted
					 * at the tick rate (5 * CO_GUEST_HZ
					 * yields, see the caller) rather than a
					 * flat count that only meant five
					 * seconds because of the old backoff
					 * ladder -- long enough to prove the
					 * freeze is gone, bounded enough to
					 * report.
					 *
					 * Unless someone is holding a terminal
					 * open, in which case idle is not the
					 * end of anything: a shell waiting for
					 * a keystroke is idle by definition,
					 * and stopping there would end the
					 * session at the prompt. The wall-clock
					 * deadline still bounds the run.
					 */
					/*
					 * vcpu->index == 0, because ctl is SHARED.
					 *
					 * The control struct is one object read by
					 * every processor, so "a secondary passes
					 * zero" was not something the caller could
					 * express -- it set idle_stop_after once
					 * and every vCPU got it. A secondary is
					 * idle most of the time by nature, so it
					 * would have reached the count first and
					 * broken out of its loop, and its host
					 * thread would have returned while the
					 * guest still had that processor online.
					 * Every on_each_cpu() after that -- which
					 * is every seccomp filter install, every
					 * static-key flip, every TLB shootdown --
					 * would spin in csd_lock_wait() forever
					 * waiting for a processor with nobody
					 * running it.
					 *
					 * Inert today because co_console_set_address()
					 * runs in the KBOOT handler before this loop
					 * starts, so the guard below is already
					 * false for every real run. That is luck,
					 * not design, and it is the wrong thing to
					 * leave a headless bring-up run standing on.
					 */
					if (vcpu->index == 0 &&
					    ctl->idle_stop_after &&
					    co_console_get_address() == 0 &&
					    out->idle_yields >= ctl->idle_stop_after) {
						co_debug("boot: %ld cooperative "
							 "idle yields, host alive "
							 "throughout -- stopping "
							 "the bring-up run",
							 out->idle_yields);
						break;
					}
					continue;
				}

				if (op == CO_OPERATION_KICK_VCPU) {
					int target = (int)pp->params[CO_PP_AP_VCPU];

					/*
					 * Only a wake. The message itself is
					 * already in the target's pending set
					 * in guest memory; this exists because
					 * a processor sitting in the host's
					 * idle wait has nothing to re-enter it.
					 */
					if (target >= 0 && target < CO_MAX_VCPUS)
						co_os_idle_wake(target);
					continue;
				}

				if (op == CO_OPERATION_START_VCPU) {
					int target = (int)pp->params[CO_PP_AP_VCPU];

					/*
					 * Hand the initial state to the thread
					 * already waiting in KVCPU_RUN, then
					 * ring its doorbell. The guest returns
					 * immediately -- it has a rendezvous of
					 * its own to wait on (cpuhp_ap_sync_
					 * alive), and blocking the boot
					 * processor inside the host would stop
					 * the very thing the new one is waiting
					 * for.
					 */
					/*
					 * No "is it active" test: the thread
					 * that will run this processor polls,
					 * so it is only briefly claimed and is
					 * usually not claimed at all when the
					 * request arrives. The request is
					 * recorded regardless and waits for it.
					 */
					if (target > 0 && target < CO_MAX_VCPUS) {
						ap_start_rip[target] = pp->params[CO_PP_AP_RIP];
						ap_start_rsp[target] = pp->params[CO_PP_AP_RSP];
						ap_start_cr3[target] = pp->params[CO_PP_AP_CR3];
						ap_start_gs[target]  = pp->params[CO_PP_AP_GS_BASE];
						/*
						 * Payload first, then the flag.
						 * volatile orders volatile
						 * accesses against each other,
						 * not plain stores against a
						 * volatile one -- without this
						 * the compiler may sink the
						 * four above past the flag and
						 * the reader can act on half a
						 * request.
						 */
						/*
						 * This processor's own extended
						 * state, for the one starting.
						 * See vcpu0_xcr0.
						 */
						vcpu0_xcr0 = pp->linuxvm_state.xcr0;
						vcpu0_cr4  = pp->linuxvm_state.cr4;

						asm volatile("" ::: "memory");
						ap_start_pending[target] = 1;
						co_os_idle_wake(target);
						co_debug("boot: starting vcpu %d at rip "
							 "0x%llx, rsp 0x%llx, cr3 0x%llx, "
							 "gs 0x%llx", target,
							 ap_start_rip[target],
							 ap_start_rsp[target],
							 ap_start_cr3[target],
							 ap_start_gs[target]);
					} else {
						co_debug_error("boot: vcpu %d cannot be "
							       "started -- no thread is "
							       "waiting for it", target);
					}
					continue;
				}

				if (op == CO_OPERATION_YIELD) {
					out->run_yields++;
					if (out->run_yields == 1)
						co_debug("boot: first cooperative run yield "
							 "after %ld switches", out->switches);
					continue;
				}

				if (op == CO_OPERATION_TERMINATE) {
					out->terminated = PTRUE;
					out->terminate_reason = pp->params[0];
					co_debug("boot: guest terminated, reason "
						 "%lld", pp->params[0]);
					/*
					 * And tell the secondaries, or the run
					 * cannot be shut down.
					 *
					 * A secondary's loop exits only on the
					 * broadcast abort, a fault, or a
					 * voluntary crossing of its own. The
					 * guest ending normally set none of
					 * those, so its threads kept looping on
					 * replayed host interrupts, pinned,
					 * inside the driver -- and the daemon's
					 * join after this call waited on them
					 * forever. taskkill cannot reach a
					 * thread in the driver; that is the
					 * stranded-run failure this port has
					 * paid for before.
					 */
					co_arch_boot_abort();
					break;
				}

				out->returned_voluntarily = PTRUE;
				out->stop_operation = op;
				co_debug("boot: unhandled operation %lld -- "
					 "stopping", op);
				break;
			}

			out->vector     = pp->params[16];
			out->fault_rip  = pp->params[5];
			out->error_code = pp->params[17];
			out->cr2        = pp->params[18];

			/*
			 * Below 32 is an exception the guest raised -- a fault,
			 * a step, a warning it meant to take. At 32 and above it
			 * is a hardware interrupt belonging to Windows, which
			 * arrives on the host's schedule and is not the guest
			 * doing anything. Only the former spends the budget.
			 */
			if (out->vector < 32)
				guest_crossings++;

			/*
			 * The operand. The stub's frame is fifteen registers
			 * pushed in a known order, so rdi is at 0x48 and rax
			 * at 0x70 -- see the layout comment on the stub.
			 *
			 * The frame is at a guest virtual address, and it is
			 * NOT dereferenced directly, although it was for the
			 * whole of bring-up and every crossing worked: the
			 * stub's frames all lived on the passage page's IST
			 * stack, mapped at the same address on both sides.
			 * The moment the guest adopted its own IDT with IST
			 * clear, frames started landing on whatever stack the
			 * kernel was running on -- the init task's boot stack
			 * was the first -- and the old direct read became a
			 * host page fault in nonpaged area, bugcheck 0x50,
			 * on the first replayed interrupt. So the frame is
			 * copied out through the guest's page tables, edited
			 * here in the buffer, and written back through the
			 * same walk by the resume paths that change it.
			 * frame == NULL means unrecorded or unreadable, and
			 * every user below already handles that.
			 */
			/*
			 * Through the root the guest was running on, not the
			 * host's load-time space.
			 *
			 * The frame lives wherever the interrupt or fault put
			 * it, and with CONFIG_VMAP_STACK that is usually a
			 * vmalloc'd task stack -- or, for an entry from ring 3,
			 * the cpu_entry_area entry stack. Both are top-level
			 * entries Linux created for itself after boot handoff,
			 * so neither exists in the PML4 the host built at load
			 * time. Read through that one the lookup simply returns
			 * NOT_FOUND, frame stays NULL, and every diagnostic
			 * below silently reports nothing -- which is how a
			 * fault on a vmap stack came to produce an empty
			 * report rather than a wrong one.
			 */
			frame = NULL;
			if (pp->params[28] &&
			    CO_OK(co_kload_read_cr3(manager,
						    pp->linuxvm_state.cr3,
						    pp->params[28],
						    (unsigned char*)frame_buf,
						    sizeof(frame_buf))))
				frame = frame_buf;

			if (frame) {
				int r;

				out->fault_rdi = frame[0x48 / 8];
				out->fault_rax = frame[0x70 / 8];
				/*
				 * The whole frame, so the last step before a
				 * stop -- fault or budget -- carries its
				 * registers out. Fifteen GPRs at frame[0..14].
				 */
				for (r = 0; r < 15; r++)
					out->stop_regs[r] = frame[r];
			}

			/*
			 * #DB with stepping on is not a fault, it is the guest
			 * having executed one instruction. Record where it was and
			 * put it straight back. The trace is a ring of the last few
			 * addresses, which is what tells you where a kernel stopped
			 * when the answer is "it stopped" rather than "it faulted".
			 */
			if (ctl->step && out->vector == 1) {
				out->steps++;

				/*
				 * One line per crossing, not per instruction.
				 *
				 * With the stub stepping thousands of
				 * instructions between crossings there is no
				 * longer any question of logging each one, and
				 * no need: the address here is where the guest
				 * had got to when its budget ran out, which is
				 * a progress mark of exactly the right
				 * granularity. The fine-grained record lives
				 * in the stub's own ring, collected below.
				 */
				/*
				 * Throttled. One record per crossing was fine
				 * at a hundred crossings and is thousands of
				 * packets at a realistic budget -- and an
				 * instrument that scales with the run is an
				 * instrument that becomes a suspect.
				 */
				if (out->switches < 8 || (out->switches & 0x3f) == 0)
					co_debug("boot: %ld steps, at 0x%llx",
						 out->steps, out->fault_rip);

				/*
				 * Re-arm the trap flag in the frame the guest is
				 * about to be resumed from.
				 *
				 * Setting TF once is not a bound. The guest can
				 * clear it with popfq or iretq -- kernels do,
				 * constantly, restoring saved flags -- and the
				 * moment it does, stepping stops and nothing
				 * limits it again. But the flags it resumes with
				 * are in a frame this code owns, so TF goes back
				 * on before every single instruction. The guest
				 * may clear it as often as it likes; it never
				 * begins an instruction without it.
				 *
				 * The frame is at params[28] and RFLAGS sits at
				 * +0x98 in it, after fifteen registers, the
				 * vector, the error code, RIP and CS. It was
				 * read into frame_buf above; the edit is made
				 * there and written back through the guest's
				 * tables.
				 */
				/*
				 * TF on, IF off, on every resume.
				 *
				 * IF matters as much as TF and for the
				 * same reason: both are the guest's to
				 * change and neither may be left to it.
				 * A guest running with interrupts
				 * enabled takes the host's interrupts
				 * through the guest's IDT, which means
				 * the host's handler has to be called
				 * by hand afterwards, off a synthesised
				 * frame, from inside a driver ioctl.
				 * Clearing it here leaves the interrupt
				 * pending until the switch back
				 * restores the host's own flags, and
				 * Windows takes it normally.
				 *
				 * The kernel will execute `sti` during
				 * boot; this undoes it before the next
				 * instruction runs.
				 */
				if (frame) {
					frame[0x98 / 8] |= 0x100ULL;
					frame[0x98 / 8] &= ~0x200ULL;
					co_kload_write_cr3(manager,
							   pp->linuxvm_state.cr3,
							   pp->params[28],
							   (const unsigned char*)frame_buf,
							   sizeof(frame_buf));
				}

				pp->linuxvm_state.return_rip = resume_rip;
				pp->linuxvm_state.rsp        = ist_top - 0x200;
				continue;
			}

			/*
			 * A warning is not a fault. WARN_ON and its relatives
			 * compile to ud2 with an entry in __bug_table, and the
			 * kernel's own #UD handler prints the warning and steps
			 * over the instruction -- two bytes -- and carries on.
			 * That handler is not installed here, because the guest
			 * runs on the host's IDT, so the host does the stepping
			 * over. Without it the first WARN_ON in the boot ends
			 * the run, which is the wrong answer to a log line.
			 *
			 * Every one is recorded. A warning stepped over silently
			 * would be worse than stopping: the kernel said
			 * something was wrong and nobody heard it.
			 *
			 * But only if the instruction actually is a warning trap,
			 * and there are two encodings of one. #UD is raised by
			 * anything the CPU refuses, and the counterexamples came
			 * quickly: xsetbv with CR4.OSXSAVE clear is three bytes,
			 * so a fixed advance of two put RIP mid-instruction and
			 * the tail faulted as garbage a step later.
			 *
			 *   ud2       0f 0b               WARN_ON and BUG, two bytes
			 *   warninsn  67 48 0f b9 3a      WARN's static call, five
			 *
			 * The second is not obvious. WARN() is a static call to
			 * __WARN_trap, and arch_static_call_transform() has a
			 * special case for that target: rather than patch the site
			 * to a call, it writes `warninsn`, a UD1 the kernel's #UD
			 * handler recognises and reports through the same bug table
			 * -- rdi already holds the bug_entry, loaded right before.
			 * So it is a warning exactly like ud2, five bytes wide, and
			 * do_one_initcall's first WARN reaches it.
			 *
			 * Read enough to tell them apart and step over the right
			 * length. Anything else reports as what it is, a real #UD
			 * at a named address.
			 */
			if (out->vector == 6) {
				static const unsigned char ud2[]      = { 0x0f, 0x0b };
				static const unsigned char warninsn[] =
					{ 0x67, 0x48, 0x0f, 0xb9, 0x3a };
				unsigned char op[5] = { 0, 0, 0, 0, 0 };
				int warn_len = 0;
				int is_halt = 0;

				if (CO_OK(co_kload_read_cr3(manager,
							    pp->linuxvm_state.cr3,
							    out->fault_rip, op,
							    sizeof(op)))) {
					/*
					 * ud2; ud2 is the cooperative halt, not a
					 * warning. native_halt and native_safe_halt
					 * emit it under co_colinux_guest, and nothing
					 * in the image is two ud2 in a row otherwise
					 * -- a single ud2 is always a WARN or BUG,
					 * followed by recovery code. The guest that
					 * reaches this has finished booting and is
					 * idle, waiting for a timer tick the host does
					 * not deliver yet. Stop and say so, rather
					 * than step over it into a spin.
					 */
					if (op[0] == ud2[0] && op[1] == ud2[1] &&
					    op[2] == ud2[0] && op[3] == ud2[1])
						is_halt = 1;
					else if (op[0] == ud2[0] && op[1] == ud2[1])
						warn_len = 2;
					else if (op[0] == warninsn[0] && op[1] == warninsn[1] &&
						 op[2] == warninsn[2] && op[3] == warninsn[3] &&
						 op[4] == warninsn[4])
						warn_len = 5;
				}

				if (is_halt) {
					out->reached_idle = PTRUE;
					co_debug("boot: guest halted at 0x%llx -- reached idle, "
						 "boot complete, after %ld switches",
						 out->fault_rip, out->switches);
					break;
				}

				if (warn_len == 0) {
					co_debug_error("boot: #UD at 0x%llx is"
						       " %02x %02x %02x %02x %02x, not a"
						       " warning trap -- the CPU refused an"
						       " instruction", out->fault_rip,
						       op[0], op[1], op[2], op[3], op[4]);
					out->faulted = PTRUE;
					break;
				}

				if (frame) {
					if (out->warnings < 8)
						out->warning_rip[out->warnings] =
							out->fault_rip;
					out->warnings++;

					co_debug("boot: warning (%d-byte) at 0x%llx,"
						 " stepping over it", warn_len,
						 out->fault_rip);

					frame[0x88 / 8] += warn_len;	/* past the trap */
					if (ctl->step) {
						frame[0x98 / 8] |= 0x100ULL;	/* TF */
						frame[0x98 / 8] &= ~0x200ULL;	/* IF */
					} else {
						/*
						 * Free-running: clear TF only.
						 * IF is left exactly as the
						 * guest had it at the trap. A
						 * free-running guest owns its
						 * interrupt flag -- forcing it
						 * off here silently disarmed
						 * interrupt forwarding at the
						 * first WARN after the kernel's
						 * local_irq_enable(), removing
						 * the one bound a free guest
						 * has.
						 */
						frame[0x98 / 8] &= ~0x100ULL;	/* no TF */
					}
					co_kload_write_cr3(manager,
							   pp->linuxvm_state.cr3,
							   pp->params[28],
							   (const unsigned char*)frame_buf,
							   sizeof(frame_buf));

					pp->linuxvm_state.return_rip = resume_rip;
					pp->linuxvm_state.rsp        = ist_top - 0x200;
					continue;
				}
			}

			/*
			 * A fault the kernel meant to take.
			 *
			 * Linux provokes faults deliberately -- a segment load
			 * with a value that may be stale, an rdmsr of a
			 * register this processor may not have, any copy to a
			 * user address -- and pairs each with an entry in
			 * __ex_table saying where to resume and what to leave
			 * in a register. Its own #GP and #PF handlers apply
			 * those entries; a cooperative guest has no such
			 * handlers, so the host applies them instead.
			 *
			 * Tried after the ud2 check above, because a WARN is
			 * also a deliberate fault and has its own table.
			 */
			if (out->vector < 32) {
				int extype = 0;

				if (frame &&
				    co_arch_extable_fixup(frame, out->vector, &extype)) {
					if (out->fixups < 8) {
						out->fixup_rip[out->fixups]  = out->fault_rip;
						out->fixup_type[out->fixups] = extype;
					}
					out->fixups++;

					if (ctl->step) {
						frame[0x98 / 8] |= 0x100ULL;	/* TF */
						frame[0x98 / 8] &= ~0x200ULL;	/* IF */
					} else {
						/* Free-running: TF only; IF is
						 * the guest's, as at the WARN
						 * step-over above. */
						frame[0x98 / 8] &= ~0x100ULL;
					}
					co_kload_write_cr3(manager,
							   pp->linuxvm_state.cr3,
							   pp->params[28],
							   (const unsigned char*)frame_buf,
							   sizeof(frame_buf));

					pp->linuxvm_state.return_rip = resume_rip;
					pp->linuxvm_state.rsp        = ist_top - 0x200;
					continue;
				}

				out->faulted = PTRUE;
				out->fault_extype = extype;
				co_debug("boot: guest exception vector %lld at rip 0x%llx, "
					 "err 0x%llx, cr2 0x%llx, after %ld switches",
					 out->vector, out->fault_rip, out->error_code,
					 out->cr2, out->switches);
				if (extype)
					co_debug_error("boot: it has an exception table entry "
						       "of type %d, which the host does not "
						       "implement", extype);
				break;
			}

			/*
			 * Every distinct vector, once. A count alone says how
			 * busy the machine was; which vectors were replayed is
			 * what says whether the host was handed something it
			 * never raises -- and that is the question a bugcheck
			 * inside Windows' own dispatch leaves you with.
			 */
			{
				unsigned v = (unsigned)(out->vector & 0xff);

				if (!vcpu->seen_vector[v]) {
					vcpu->seen_vector[v] = 1;
					co_debug("boot: vcpu %d replaying host interrupt "
						 "vector 0x%x (%u) through the live Windows "
						 "IDT, after %ld switches",
						 vcpu->index, v, v, out->switches);
				}
			}

			if (!co_arch_forward_host_interrupt(pp->host_state.idt.table,
							    out->vector)) {
				/*
				 * Not forwardable -- an IST gate, or absent. Stop
				 * rather than drop it silently and carry on with
				 * the host missing an interrupt it needed.
				 */
				out->unforwardable = PTRUE;
				break;
			}
			out->interrupts++;

			/*
			 * Back in, exactly where it was.
			 *
			 * The rsp handed to the switch is scratch -- co_guest_resume
			 * replaces it immediately with the saved frame pointer -- but
			 * the switch still pushes CS and the return RIP onto it before
			 * the lretq, so it must not land inside the frame. The frame
			 * occupies ist_top-0xb0 up to ist_top-0x08, so anything in
			 * that span writes over saved registers: ist_top-0x40 would
			 * have overwritten rbx and rcx on every single resume, and the
			 * guest would have come back subtly wrong rather than
			 * obviously broken.
			 */
			/*
			 * The cooperative timer, delivered here because this is
			 * the only moment the host holds a running guest still.
			 *
			 * The guest takes no asynchronous entry of its own: this
			 * interrupt vectored through its IDT into our stub and has
			 * just been replayed into Windows, and not one guest
			 * instruction ran for it. Left alone the guest resumes
			 * exactly where it was -- which is why a task that stays
			 * runnable without entering the kernel could never be taken
			 * off the processor, and why one such task froze the whole
			 * guest permanently, with its clock stopped, its receive
			 * thread asleep on a timer that would never fire, and this
			 * loop spinning at full speed because no idle yield ever
			 * came.
			 *
			 * So when a tick is due, resume the guest's timer entry
			 * over a frame built to return where it was afterwards.
			 * What makes this safe is that it is not a hand-written
			 * entry: asm_sysvec_co_timer is a stock idtentry, so
			 * error_entry makes the swapgs decision from the
			 * interrupted CS and irqentry_exit does the scheduling and
			 * the return-to-user work. Every part of delivering an
			 * interrupt that is easy to get wrong is already written
			 * and tested in the guest.
			 */
			co_arch_inject_tick(manager, pp, ctl, vcpu->index,
					    &tick_last, out);

			pp->linuxvm_state.return_rip = resume_rip;
			pp->linuxvm_state.rsp        = ist_top - 0x200;
		}

		out->guest_switches = guest_crossings;
	}

	return CO_RC(OK);
}

/*
 * Boot a guest on the boot processor, and own everything a run allocates.
 *
 * Three steps now, where there was one 1550-line function: prepare the machine,
 * run this processor's crossing loop, give it all back. The middle step is the
 * one a secondary shares (co_arch_test_smp_lane calls the same function with
 * its own vCPU and the same control struct), which is what stopped the two
 * kinds of processor from drifting apart operation by operation.
 */
co_rc_t co_arch_boot_loaded(co_manager_t* manager, co_arch_guest_space_t* space,
			    co_arch_boot_t* in, co_arch_boot_result_t* out)
{
	co_boot_ctx_t ctx = { NULL, NULL, 0 };
	co_arch_passage_page_t* pp;
	struct co_console_ring* ring;
	co_vcpu_t* vcpu;
	int pages = sizeof(co_arch_passage_page_t) / CO_ARCH_PAGE_SIZE;
	unsigned long granted;
	int n;
	co_rc_t rc;

	rc = co_arch_boot_prepare(manager, space, in, out, &ctx);
	if (!CO_OK(rc))
		goto cleanup;

	pp   = ctx.pp;
	ring = ctx.ring;

	/*
	 * vCPU 0. The boot processor is the one that comes up through KBOOT,
	 * and the crossing state it owns is reached through the vCPU, so a
	 * second one is an index rather than a rewrite.
	 */
	vcpu = co_vcpu_claim(0);
	if (vcpu == NULL) {
		rc = CO_RC(ERROR);
		goto cleanup;
	}
	vcpu->pp = pp;
	/*
	 * Publish the address space for the secondaries. By here CR3 is the
	 * adopted kernel root, which is the one that stays valid -- the guest
	 * may switch to per-process tables as often as it likes afterwards, and
	 * they all share the kernel half of this one, which is where the
	 * passage pages live.
	 */
	vcpu->guest_cr3         = pp->linuxvm_state.cr3;
	vcpu->passage_symbol_va = in->passage_symbol_va;

	/*
	 * Start the async block workers before the guest runs. If it fails, or
	 * async is off (--sync-cobd), the loop falls back to inline transfers --
	 * co_cobd_async_submit is simply never called because the BLOCK_IO
	 * branch checks the control struct.
	 */
	if (in->async_cobd && in->cobd_io_va) {
		if (!CO_OK(co_cobd_async_start(manager, in->cobd_io_va))) {
			co_debug("boot: async cobd unavailable -- "
				 "falling back to inline block I/O");
			in->async_cobd = 0;
		}
	}

	/*
	 * The control struct, built once and read-only from here.
	 *
	 * Written after the async fallback above has settled, so a secondary and
	 * the boot processor cannot disagree about whether async is in effect,
	 * and before any secondary can be started -- co_arch_test_smp_lane reads
	 * this same object with no lock, which is only safe because nothing
	 * writes it once a processor other than this one exists.
	 */
	vcpu_ctl.max_switches  = (unsigned long)in->max_switches;
	vcpu_ctl.step          = in->step;
	vcpu_ctl.batch         = in->batch;
	vcpu_ctl.tick_entry_va = in->tick_entry_va;
	vcpu_ctl.virtual_if_va = in->virtual_if_va;
	vcpu_ctl.async_cobd    = in->async_cobd;
	vcpu_ctl.cobd_io_va    = in->cobd_io_va;
	/*
	 * A guest nobody is talking to gets the bring-up bound; one with a
	 * terminal attached gets long enough to be used, since a person at a
	 * prompt is the slowest thing in the system.
	 */
	vcpu_ctl.deadline_secs = co_console_get_address()
		? CO_BOOT_CONSOLE_SECONDS : CO_BOOT_MAX_SECONDS;
	/*
	 * The headless bring-up stop, restored and re-derived.
	 *
	 * It was a flat 500 slept yields, which stood for "about five seconds of
	 * an idle guest" only because of the backoff ladder: a hundred yields at
	 * one millisecond and the rest at ten. Retiring it entirely was wrong for
	 * the same reason the indefinite wait was -- it assumed a yield no longer
	 * corresponded to a fixed slice of time, when in fact every yield is now
	 * exactly one tick period.
	 *
	 * So it goes back, expressed as what it always meant: seconds of idle,
	 * converted at the tick rate. Five seconds is long enough to prove an
	 * idle guest is not freezing the host and short enough to report, which
	 * is what the number was for. A secondary passes zero: a run reports
	 * through the boot processor, and a secondary that stopped counting idle
	 * yields of its own would leave the guest a processor short with nothing
	 * said about it.
	 *
	 * The wall-clock deadline above remains the real backstop.
	 */
	vcpu_ctl.idle_stop_after = 5 * CO_GUEST_HZ;

	/*
	 * A finer host clock for the duration of the run.
	 *
	 * Every sleep the loop takes is rounded up to the host's clock tick, and
	 * it sleeps whenever the guest is idle -- which a guest waiting for a
	 * reply is. So the tick is the floor on the guest's round-trip time, and
	 * at XP's default 15.6 ms that is the whole story of the network's
	 * speed: a ping to slirp's gateway, answered by a process on this same
	 * machine, measured 10 ms, and a download ran at the same rate from a
	 * mirror 5 ms away as from one on another continent.
	 *
	 * Claimed here and released below rather than left on, because it is a
	 * system-wide setting and a faster clock costs the whole machine power
	 * and interrupts. One acquire per run, not per processor: it is
	 * reference counted and system-wide, so there is nothing for a secondary
	 * to add.
	 */
	granted = co_os_timer_resolution_acquire();
	co_debug("boot: timer resolution %lu.%01lu ms",
		 granted / 10000, (granted / 1000) % 10);

	rc = co_arch_vcpu_run(manager, vcpu, &vcpu_ctl, out);

	{
		/*
		 * Join the async workers before the vCPU goes inactive, and
		 * before this function frees the stack and passage page below --
		 * and well before guest RAM is freed at KLOAD_END. A worker's
		 * page-table walk can then never outlive the memory it walks.
		 * Releasing the vCPU only after the join means a driver unload
		 * waiting on co_arch_boot_running cannot proceed until the
		 * workers are gone either.
		 */
		/*
		 * And the secondaries before the workers, for the same reason
		 * the workers come before the frees.
		 *
		 * A secondary answers BLOCK_IO and BLOCK_PROBE out of its own
		 * lane, so until its lane has ended it can submit into the
		 * cobd machinery at any moment. Stopping the workers while one
		 * is still crossing is a submission into state that is being
		 * torn down -- a page fault in non-paged memory, taken during
		 * shutdown, which is exactly what this cost before the wait
		 * was here.
		 *
		 * vcpu_abort_all is already set by the TERMINATE handler, so
		 * every lane is on its way out; this only declines to proceed
		 * until they have arrived. Bounded, because a lane that will
		 * not end is a worse problem than a lane that ended late, and
		 * pressing on at least leaves a report.
		 */
		{
			int spins, v, live;

			for (spins = 0; spins < 500; spins++) {
				live = 0;
				for (v = 1; v < CO_MAX_VCPUS; v++)
					if (co_vcpu[v].active)
						live++;
				if (!live)
					break;
				/*
				 * Every doorbell, not co_os_idle_wake(v).
				 *
				 * v is CO_MAX_VCPUS here -- the value that ended
				 * the scan immediately above -- and
				 * co_os_idle_wake() rejects anything at or past
				 * that bound. So this line has never woken
				 * anything at all; the loop worked only because a
				 * secondary's poll timeout expired on its own and
				 * it noticed vcpu_abort_all by itself.
				 *
				 * That accident is about to stop being available:
				 * with the idle wait indefinite, a secondary that
				 * is not rung parks forever, this loop spins its
				 * full five seconds, logs "still running", and
				 * then out_free_pp frees ap_passage[] out from
				 * under a processor that is still crossing
				 * through it.
				 */
				co_os_idle_wake_all();
				co_os_msleep(10);
			}

			if (live)
				co_debug_error("boot: %d secondary lane(s) still "
					       "running at teardown", live);
		}

		if (in->async_cobd && in->cobd_io_va) {
			co_cobd_async_stop();
			out->block_errors += co_cobd_async_errors();
		}

		co_vcpu_release(vcpu);

		/*
		 * Give the clock back. Paired with the acquire above: the host
		 * reference counts it, and leaving it claimed would keep the
		 * whole machine on a fast tick after the guest has gone.
		 */
		co_os_timer_resolution_release();

		/*
		 * The stub's ring, which is where the fine-grained trace lives
		 * now. It records every instruction it stepped, so these are the
		 * last sixteen the guest executed rather than the last sixteen
		 * the host happened to see.
		 */
		{
			int t;

			out->trace_next = pp->params[47];
			for (t = 0; t < CO_BOOT_TRACE; t++)
				out->trace[t] = pp->params[31 + t];
		}
	}

	co_debug("boot: loop ended -- %ld switches, %ld steps, %ld interrupts, "
		 "%ld fixups, %lld bytes printed", out->switches, out->steps,
		 out->interrupts, out->fixups, ring->written);

	out->console_written  = ring->written;
	out->console_capacity = ring->capacity;
	for (n = 0; n < (int)sizeof(out->console_text) - 1 && n < (int)ring->written; n++)
		out->console_text[n] = ring->text[n];
	out->console_text[n] = 0;

	rc = CO_RC(OK);

cleanup:
	/*
	 * One cleanup, reached from every exit.
	 *
	 * This replaces out_free_stack falling into out_free_pp. Both halves are
	 * unconditional now because ctx records each allocation the moment it
	 * exists and holds NULL or zero otherwise -- so a prepare that failed at
	 * its second step gives back exactly what its first step took, without
	 * anyone having to pick the right label to jump to. That choice is what
	 * the old arrangement got wrong every time an early exit was added.
	 */
	co_arch_extable_free();
	if (ctx.stack_pfn)
		co_os_put_page(manager, ctx.stack_pfn);
	/*
	 * The secondaries' pages too. Nothing else releases them: they are
	 * reached only through a static array, which the next boot merely
	 * NULLs. Fifteen executable non-paged pages per secondary leaked per
	 * run is the strand-memory-per-run failure this port already has a
	 * scar from.
	 */
	{
		int v;

		for (v = 1; v < CO_MAX_VCPUS; v++) {
			if (ap_passage[v] == NULL)
				continue;
			co_os_free_exec_pages(ap_passage[v], pages);
			ap_passage[v] = NULL;
		}
	}

	if (ctx.pp)
		co_os_free_exec_pages(ctx.pp, pages);

	return rc;
}
