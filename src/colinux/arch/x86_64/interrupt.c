/*
 * This source code is a part of coLinux source package.
 *
 * The code is licensed under the GPL. See the COPYING file at
 * the root directory.
 */

/* GCC's own freestanding stddef.h, for size_t as a pointer-sized integer. */
#include <stddef.h>

#include <colinux/arch/interrupt.h>

/*
 * Forwarding a hardware interrupt that arrived while the guest was running.
 *
 * UNEXERCISED. Nothing reaches this code yet: co_host_switch_wrapper() below
 * refuses to switch, so no guest ever runs and no interrupt is ever forwarded.
 * It cannot be tested before a guest boots (M4), which makes it the most likely
 * place in this directory to be quietly wrong. Review it on paper before
 * trusting it, and treat the alignment reasoning in call_intr() as a claim to be
 * checked rather than a fact.
 */

/*
 * A long-mode interrupt gate is 16 bytes and splits the handler address across
 * three fields, where the i386 gate is 8 bytes and splits it across two.
 */
struct x86_64_idt_gate {
	unsigned short offset_low;
	unsigned short selector;
	unsigned short flags;		/* ist in bits 0-2, then type/dpl/present */
	unsigned short offset_mid;
	unsigned int   offset_high;
	unsigned int   reserved;
} __attribute__((packed));

typedef char co_assert_idt_gate_size
	[(sizeof(struct x86_64_idt_gate) == 16) ? 1 : -1];

static inline void call_intr(void *func)
{
	/*
	 * iretq unconditionally pops five quadwords -- RIP, CS, RFLAGS, RSP, SS --
	 * even returning to the same privilege level, where the i386 iret pops only
	 * three. So the synthesised frame is 40 bytes and has to carry a valid RSP
	 * and SS, not just flags/cs/eip.
	 *
	 * On taking an interrupt in long mode the CPU aligns RSP to 16 bytes before
	 * pushing that frame, so a real handler is entered with RSP congruent to 8
	 * modulo 16. Align before pushing so the handler sees what it expects; the
	 * original RSP is restored from the frame by the handler's own iretq.
	 */
	asm volatile(
	    "    movq %%rsp, %%r11"                  "\n"  /* rsp to return to */
	    "    andq $-16, %%rsp"                   "\n"  /* match hardware entry */
	    "    movl %%ss, %%eax"                   "\n"
	    "    pushq %%rax"            /* SS     */ "\n"
	    "    pushq %%r11"            /* RSP    */ "\n"
	    "    pushfq"                 /* RFLAGS */ "\n"
	    "    movl %%cs, %%eax"                   "\n"
	    "    pushq %%rax"            /* CS     */ "\n"
	    "    leaq 2f(%%rip), %%rax"               "\n"
	    "    pushq %%rax"            /* RIP    */ "\n"
	    "    jmp *%0"                             "\n"
	    "2:  sti"                                 "\n"
	    : : "r"(func) : "rax", "r11", "memory");
}

void co_monitor_arch_real_hardware_interrupt(co_monitor_t *cmon)
{
	struct x86_64_idt_gate *host;
	unsigned long long offset;
	void *func;

	host = (struct x86_64_idt_gate *)(cmon->passage_page->host_state.idt.table);
	host = &host[co_passage_page->params[0]];

	offset = (unsigned long long)host->offset_low
	       | ((unsigned long long)host->offset_mid  << 16)
	       | ((unsigned long long)host->offset_high << 32);

	/*
	 * The gate's IST index is ignored here. A non-zero IST means the CPU would
	 * have switched to a dedicated stack from the TSS before entering the
	 * handler, which this synthesised entry does not do. Windows uses IST for
	 * machine check, NMI and double fault -- none of which should be arriving
	 * through this path -- but it is a real gap to close alongside the rest.
	 */
	/*
	 * (size_t), not (unsigned long): Win64 is LLP64 and `unsigned long` is
	 * four bytes, which would truncate the host ISR's address to its low
	 * half and jump into nothing. Every Windows handler is above 4 GB.
	 */
	func = (void *)(size_t)offset;

	call_intr(func);
}

void co_monitor_arch_enable_interrupts(void)
{
	asm("sti\n");
}
