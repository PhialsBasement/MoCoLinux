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
#include <colinux/arch/switch.h>
#include <colinux/arch/state.h>
#include <colinux/arch/space.h>

#include "mmu.h"
#include "utils.h"
#include "defs.h"

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
#define CO_PP_HOST_STATE	"0x640"
#define CO_PP_LINUXVM_STATE	"0x780"
#define CO_PP_FAULTED		"0x8e8"
#define CO_PP_FAULT_RIP		"0x8f0"

#define CO_PP_HOST_STATE_N	0x640
#define CO_PP_LINUXVM_STATE_N	0x780
#define CO_PP_FAULTED_N		0x8e8
#define CO_PP_FAULT_RIP_N	0x8f0

/* params[6] and params[7]; params[8..15] are reserved for the guest GDT. */
#define CO_PP_COUNTER		"0x8f8"
#define CO_PP_REGSUM		"0x900"
#define CO_PP_COUNTER_N		0x8f8
#define CO_PP_REGSUM_N		0x900

/*
 * What a fault leaves behind, at params[16..18]. Placed above the GDT's eight
 * reserved slots so a TSS descriptor can be added there without moving these --
 * the stubs reach them by literal offset and cannot be recompiled per layout.
 */
#define CO_PP_VECTOR		"0x948"
#define CO_PP_ERRCODE		"0x950"
#define CO_PP_CR2		"0x958"
#define CO_PP_VECTOR_N		0x948
#define CO_PP_ERRCODE_N		0x950
#define CO_PP_CR2_N		0x958

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
#define CO_PP_CALL_TARGET	"0x968"
#define CO_PP_CALL_ARG0		"0x970"
#define CO_PP_CALL_ARG1		"0x978"
#define CO_PP_CALL_ARG2		"0x980"
#define CO_PP_CALL_RET		"0x988"
#define CO_PP_CALL_ARG3		"0x990"
#define CO_PP_CALL_ARG4		"0x998"
#define CO_PP_CALL_ARG5		"0x9a0"

/*
 * params[28]: where the interrupted guest's register frame was left.
 *
 * A guest that is going to be resumed cannot have its registers thrown away,
 * and until the guest kernel has coLinux's own interrupt entry hooks -- which
 * would do SAVE_ALL themselves -- the stub has to preserve them. It pushes the
 * full set onto the IST stack, records rsp here, and the resume path pops them
 * and returns with iretq into the exact instruction that was interrupted.
 */
#define CO_PP_GUEST_FRAME	"0x9a8"
#define CO_PP_GUEST_FRAME_N	0x9a8

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
#define CO_PP_STEP		"0x9b0"
#define CO_PP_STEP_N		0x9b0
#define CO_PP_CALL_TARGET_N	0x968
#define CO_PP_CALL_RET_N	0x988

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
    /*
     * The GDT has to travel with the address space. lretq does not merely set
     * CS:RIP, it loads a segment descriptor -- so it reads the GDT. Leaving GDTR
     * pointing at the host's table across the CR3 write means that read lands on
     * an unmapped page, and the page-fault handler is unmapped too: double fault,
     * triple fault, instant reset with no bugcheck. Learned the hard way.
     */
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
    /* drop global TLB entries, or the old address space stays visible */
    "    mov %cr4, %rax                                             \n"
    "    mov %rax, %r10                                             \n"
    "    btr $7, %rax                                               \n"
    "    mov %rax, %cr4                                             \n"
    /* --- the crossing --- */
    "    mov " CO_ARCH_STATE_STACK_CR3 "(%rdx), %rax                \n"
    "    mov %rax, %cr3                                             \n"
    "    mov %r10, %rax                                             \n"
    "    mov %rax, %cr4                                             \n"
    /*
     * Now in the other address space, so its GDT is reachable. This must come
     * after the CR3 write and before the lretq that reads it.
     */
    "    lgdt " CO_ARCH_STATE_STACK_GDT "(%rdx)                     \n"
    "    lidt " CO_ARCH_STATE_STACK_IDT "(%rdx)                     \n"
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
     * rax and r11 only: rcx, rdx, r8 and r9 carry state across the far return,
     * and r10 is holding CR4.
     */
    "    movzwl " CO_ARCH_STATE_STACK_TR "(%rdx), %r11d             \n"
    "    test %r11d, %r11d                                          \n"
    "    jz 3f                                                      \n"
    "    mov " CO_PP_GDT_BASE "(%rdx), %rax                         \n"
    "    and $-8, %r11d                                             \n"
    "    add %r11, %rax                                             \n"
    "    andl $0xfffffdff, 4(%rax)     /* clear the busy bit */     \n"
    "    ltr " CO_ARCH_STATE_STACK_TR "(%rdx)                       \n"
    "3:                                                             \n"
    /* onto the other side's stack, then far-return to its cs:rip */
    "    mov " CO_ARCH_STATE_STACK_RSP "(%rdx), %rsp                \n"
    "    push " CO_ARCH_STATE_STACK_CS "(%rdx)                      \n"
    "    push " CO_ARCH_STATE_STACK_RETURN_RIP "(%rdx)              \n"
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
    "    sti                                                        \n"
    "    cmpq $0, " CO_PP_STEP "(%rax)                              \n"
    "    je 4f                                                      \n"
    /*
     * Set TF last. Every instruction after this one traps, so anything between
     * here and the jump would cost a world switch for nothing.
     */
    "    pushfq                                                     \n"
    "    orq $0x100, (%rsp)                                         \n"
    "    popfq                                                      \n"
    "4:  jmp *%r10                                                  \n"
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
#define CO_PP_SWITCH_ENTRY_N	0x960


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
	co_pa_t first_pa, pt_pa, pd_pa, pdpt_pa;
	unsigned long long last_va = va + sizeof(*pp) - 1;

	/*
	 * One PT is installed, at one PD slot, so all 15 pages have to fall inside the
	 * same 2 MB region. NonPagedPool has no reason to honour that, and when it does
	 * not, CO_ARCH_PTE_INDEX wraps and the tail pages are quietly mapped nowhere --
	 * which the guest only discovers by faulting on an unmapped stack or IDT. Refuse
	 * instead, so a bad allocation is a failed test and not a reset.
	 */
	if (CO_ARCH_PMD_INDEX(va)  != CO_ARCH_PMD_INDEX(last_va) ||
	    CO_ARCH_PUD_INDEX(va)  != CO_ARCH_PUD_INDEX(last_va) ||
	    CO_ARCH_PGD_INDEX(va)  != CO_ARCH_PGD_INDEX(last_va)) {
		co_debug_error("passage page 0x%llx..0x%llx straddles a 2MB boundary",
			       va, last_va);
		return PFALSE;
	}

	co_memset(&pp->guest_temp, 0, sizeof(pp->guest_temp));

	first_pa = co_os_virt_to_phys(&pp->first_page);
	pt_pa    = co_os_virt_to_phys(&pp->guest_temp.pt[0]);
	pd_pa    = co_os_virt_to_phys(&pp->guest_temp.pd[0]);
	pdpt_pa  = co_os_virt_to_phys(&pp->guest_temp.pdpt[0]);

	/*
	 * Map every page of the passage page, not just the first. The guest needs a
	 * stack to be far-returned onto and to push from, and the only memory it can
	 * touch is what this table maps. NonPagedPool is not physically contiguous,
	 * so each page is looked up individually.
	 */
	{
		int page;

		for (page = 0; page < (int)(sizeof(*pp) / CO_ARCH_PAGE_SIZE); page++) {
			unsigned char* p = (unsigned char*)pp + page * CO_ARCH_PAGE_SIZE;
			unsigned long long page_va = va + page * CO_ARCH_PAGE_SIZE;

			pp->guest_temp.pt[0][CO_ARCH_PTE_INDEX(page_va)] =
				co_os_virt_to_phys(p) | _KERNPG_TABLE;
		}
	}

	pp->guest_temp.pt[0][CO_ARCH_PTE_INDEX(va)]   = first_pa | _KERNPG_TABLE;
	pp->guest_temp.pd[0][CO_ARCH_PMD_INDEX(va)]   = pt_pa    | _KERNPG_TABLE;
	pp->guest_temp.pdpt[0][CO_ARCH_PUD_INDEX(va)] = pd_pa    | _KERNPG_TABLE;
	pp->guest_temp.pml4[CO_ARCH_PGD_INDEX(va)]    = pdpt_pa  | _KERNPG_TABLE;

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
	  __builtin_offsetof(co_arch_passage_page_t, params) + 29 * 8 == CO_PP_STEP_N)
	 ? 1 : -1];

/* The blob reaches gdt.base by literal offset; keep it honest. */
typedef char co_assert_gdt_base_offset
	[(__builtin_offsetof(co_arch_state_stack_t, gdt) + 2 == CO_PP_GDT_BASE_N) ? 1 : -1];

/* The stubs need a page to themselves, after the IDT's. */
typedef char co_assert_stubs_fit_host_temp
	[(sizeof(((co_arch_passage_page_t*)0)->host_temp)
	  >= (CO_PP_TSS_PAGE + 1) * 0x1000) ? 1 : -1];
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
static co_arch_passage_page_t* co_setup_guest_page(co_arch_switch_test_t* out,
						   unsigned long entry_offset)
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

	pp = co_os_alloc_exec_pages(pages);
	if (pp == NULL)
		return NULL;

	co_memset(pp, 0, sizeof(*pp));

	va = (unsigned long long)(size_t)pp;
	if (!co_build_guest_tables(pp, va)) {
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
		unsigned long long* guest_gdt = &pp->params[8];
		unsigned char* host_temp = (unsigned char*)&pp->host_temp;
		struct co_x86_64_tss* tss = (struct co_x86_64_tss*)
			(host_temp + CO_PP_TSS_PAGE * CO_ARCH_PAGE_SIZE);
		unsigned long long ist_top = (unsigned long long)(size_t)
			(host_temp + (CO_PP_ISTSTACK_PAGE + 1) * CO_ARCH_PAGE_SIZE);

		co_memset(tss, 0, sizeof(*tss));
		tss->ist[0]     = ist_top;	/* IST1: a whole page, growing down */
		tss->iomap_base = sizeof(*tss);	/* past the limit: no I/O bitmap */

		guest_gdt[0] = 0x0000000000000000ULL;	/* null */
		guest_gdt[1] = 0x00209a0000000000ULL;	/* 64-bit code, selector 0x08 */
		co_build_tss_descriptor(&guest_gdt[2],
					(unsigned long long)(size_t)tss,
					sizeof(*tss) - 1);	/* selector 0x10 */

		pp->linuxvm_state.gdt.base  = (struct x86_dt_entry*)guest_gdt;
		pp->linuxvm_state.gdt.limit = (4 * 8) - 1;
		pp->linuxvm_state.cs        = 0x08;
		pp->linuxvm_state.tr        = 0x10;

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
		co_build_guest_idt(idt, stubs_va, 0x08);

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
	unsigned long long stack_va = 0xffffffff90000000ULL;
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
	unsigned long long stack_va = 0xffffffff90000000ULL;
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
bool_t co_arch_forward_host_interrupt(void* host_idt, unsigned long long vector)
{
	struct co_x86_64_gate* idt = (struct co_x86_64_gate*)host_idt;
	struct co_x86_64_gate* gate;
	unsigned long long offset;
	void* func;

	if (idt == NULL || vector > 255)
		return PFALSE;

	gate = &idt[vector];

	if (!(gate->flags & 0x8000))		/* not present */
		return PFALSE;

	/*
	 * Refuse gates that ask for an IST stack.
	 *
	 * A non-zero IST index means the hardware path would have switched to a
	 * dedicated stack out of the TSS before entering the handler, and this
	 * synthesised entry does not. Windows uses IST for NMI, machine check and
	 * double fault -- precisely the handlers where running on the wrong stack
	 * turns a recoverable event into an unrecoverable one. Report rather than
	 * call: losing an NMI is bad, corrupting whatever is under the current
	 * stack pointer while handling one is worse.
	 */
	if (gate->flags & 0x7)
		return PFALSE;

	offset = (unsigned long long)gate->offset_low
	       | ((unsigned long long)gate->offset_mid  << 16)
	       | ((unsigned long long)gate->offset_high << 32);

	/* (size_t): unsigned long is four bytes here and every ISR is above 4 GB. */
	func = (void*)(size_t)offset;

	asm volatile(
	    "    movq %%rsp, %%r11"		"\n"
	    "    andq $-16, %%rsp"		"\n"	/* as hardware aligns it */
	    "    movl %%ss, %%eax"		"\n"
	    "    pushq %%rax"			"\n"	/* SS     */
	    "    pushq %%r11"			"\n"	/* RSP    */
	    "    pushfq"			"\n"	/* RFLAGS */
	    "    movl %%cs, %%eax"		"\n"
	    "    pushq %%rax"			"\n"	/* CS     */
	    "    leaq 1f(%%rip), %%rax"		"\n"
	    "    pushq %%rax"			"\n"	/* RIP    */
	    "    cli"				"\n"
	    "    jmp *%0"			"\n"
	    "1:"				"\n"
	    : : "r"(func) : "rax", "r11", "memory", "cc");

	return PTRUE;
}

co_rc_t co_arch_boot_loaded(co_manager_t* manager, co_arch_guest_space_t* space,
			    co_arch_boot_t* in, co_arch_boot_result_t* out)
{
	co_arch_passage_page_t* pp;
	co_arch_switch_test_t setup = {0, };
	co_switch_full_fn fn;
	struct co_console_ring* ring;
	unsigned long long ring_va, stack_va = 0xffffffff90000000ULL;
	co_pfn_t stack_pfn = 0;
	int pages = sizeof(co_arch_passage_page_t) / CO_ARCH_PAGE_SIZE;
	int page, n;
	co_rc_t rc;

	co_memset(out, 0, sizeof(*out));
	out->supported = PTRUE;

	if (space == NULL || !in->entry_va || !in->initial_code_va || !in->start_kernel_va)
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

	ring = (struct co_console_ring*)((unsigned char*)&pp->host_temp
					 + CO_PP_CONSOLE_PAGE * CO_ARCH_PAGE_SIZE);
	ring_va = (unsigned long long)(size_t)ring;
	co_memset(ring, 0, CO_ARCH_PAGE_SIZE);
	ring->capacity = CO_ARCH_PAGE_SIZE - sizeof(*ring);

	if (!CO_OK(co_write_guest_u64(manager, space, in->ring_symbol_va, ring_va)) ||
	    !CO_OK(co_write_guest_u64(manager, space, in->early_console_va,
				      in->colinux_console_va)) ||
	    !CO_OK(co_write_guest_u64(manager, space, in->initial_code_va,
				      in->start_kernel_va)) ||
	    (in->guest_flag_va &&
	     !CO_OK(co_write_guest_u64(manager, space, in->guest_flag_va, 1)))) {
		co_debug_error("could not write the boot globals");
		rc = CO_RC(ERROR);
		goto out_free_stack;
	}

	pp->linuxvm_state.cr3 = co_arch_guest_space_root(space);
	pp->params[19]        = (unsigned long long)(size_t)pp->code;

	/*
	 * Enter through the boot shim so the guest runs with interrupts enabled,
	 * and point it at the kernel's entry.
	 */
	pp->params[20] = in->entry_va;
	pp->params[29] = in->step ? 1 : 0;
	pp->linuxvm_state.return_rip = (unsigned long long)(size_t)pp->code
		+ (unsigned long)(&co_boot_shim - &co_switch_full);
	pp->linuxvm_state.rsp = stack_va + CO_ARCH_PAGE_SIZE - 0x40;

	fn = (co_switch_full_fn)(void*)pp->code;

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
		rc = CO_RC(ERROR);
		goto out_free_stack;
	}
	out->preflight_checked = setup.preflight_checked;

	/*
	 * The monitor loop.
	 *
	 * The guest runs until something interrupts it. External vectors (32 and
	 * above) are the host's -- forward them to Windows and put the guest back
	 * exactly where it was. Anything below 32 is an exception the guest itself
	 * took, which is news, so stop and report it.
	 *
	 * Bounded, because an unbounded loop here is the same bug as an unbounded
	 * guest: this runs inside a driver ioctl and has to give the thread back.
	 */
	{
		unsigned long long resume_rip = (unsigned long long)(size_t)pp->code
			+ (unsigned long)(&co_guest_resume - &co_switch_full);
		unsigned long long ist_top = (unsigned long long)(size_t)&pp->host_temp
			+ (CO_PP_ISTSTACK_PAGE + 1) * CO_ARCH_PAGE_SIZE;
		int i;

		for (i = 0; i < in->max_switches; i++) {
			pp->params[4] = 0;		/* faulted */

			fn(&pp->host_state, &pp->linuxvm_state, NULL, 0);

			out->switches++;

			if (!pp->params[4]) {
				/* came back on its own -- nothing here does that yet */
				out->returned_voluntarily = PTRUE;
				break;
			}

			out->vector     = pp->params[16];
			out->fault_rip  = pp->params[5];
			out->error_code = pp->params[17];
			out->cr2        = pp->params[18];

			/*
			 * #DB with stepping on is not a fault, it is the guest
			 * having executed one instruction. Record where it was and
			 * put it straight back. The trace is a ring of the last few
			 * addresses, which is what tells you where a kernel stopped
			 * when the answer is "it stopped" rather than "it faulted".
			 */
			if (in->step && out->vector == 1) {
				out->steps++;
				out->trace[out->trace_next & (CO_BOOT_TRACE - 1)] =
					out->fault_rip;
				out->trace_next++;

				pp->linuxvm_state.return_rip = resume_rip;
				pp->linuxvm_state.rsp        = ist_top - 0x200;
				continue;
			}

			if (out->vector < 32) {
				out->faulted = PTRUE;
				break;
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
			pp->linuxvm_state.return_rip = resume_rip;
			pp->linuxvm_state.rsp        = ist_top - 0x200;
		}

		if (out->switches >= in->max_switches)
			out->hit_limit = PTRUE;
	}

	out->console_written  = ring->written;
	out->console_capacity = ring->capacity;
	for (n = 0; n < (int)sizeof(out->console_text) - 1 && n < (int)ring->written; n++)
		out->console_text[n] = ring->text[n];
	out->console_text[n] = 0;

	rc = CO_RC(OK);

out_free_stack:
	if (stack_pfn)
		co_os_put_page(manager, stack_pfn);
out_free_pp:
	co_os_free_exec_pages(pp, pages);

	return rc;
}
