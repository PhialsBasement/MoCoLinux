/*
 * This source code is a part of coLinux source package.
 *
 * The code is licensed under the GPL. See the COPYING file at
 * the root directory.
 */

/*
 * Forwarding a hardware interrupt that arrived while the guest was running.
 *
 * This used to carry its own copy of the gate decoding and the synthesised
 * interrupt frame, written from the i386 version and never run. It is now a
 * caller of co_arch_forward_host_interrupt(), which is the same code the
 * monitor loop uses and which has actually been exercised.
 *
 * Two copies of this in the tree was the same mistake as two copies of the
 * guest-page setup: the one that runs gets fixed and the one that does not
 * quietly stops matching it, and this is not code where "quietly wrong" is
 * survivable.
 */

/* GCC's own freestanding stddef.h, for size_t as a pointer-sized integer. */
#include <stddef.h>

#include <colinux/arch/interrupt.h>
#include <colinux/arch/switch.h>

void co_monitor_arch_real_hardware_interrupt(co_monitor_t *cmon)
{
	/*
	 * The vector the guest recorded, and the host's own IDT as it was when
	 * the switch saved it on the way out.
	 */
	co_arch_forward_host_interrupt(cmon->passage_page->host_state.idt.table,
				       co_passage_page->params[0]);
}

void co_monitor_arch_enable_interrupts(void)
{
	asm("sti\n");
}
