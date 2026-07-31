/*
 * This source code is a part of coLinux source package.
 *
 * The code is licensed under the GPL. See the COPYING file at
 * the root directory.
 */

#ifndef __COLINUX_ARCH_SWITCH_H__
#define __COLINUX_ARCH_SWITCH_H__

#include <colinux/common/common.h>
#include <colinux/kernel/manager.h>

/*
 * Result of changing CR3 into a minimal guest address space and continuing to
 * execute there. See arch/x86_64/switch.c.
 */
typedef struct {
	int		   supported;
	int		   succeeded;
	unsigned long long passage_va;
	unsigned long long passage_pa;
	unsigned long long code_va;
	unsigned long long host_cr3;
	unsigned long long guest_cr3;
	unsigned long long expected;
	unsigned long long observed;
	unsigned long long guest_gdt;
	unsigned long long guest_idt;
	unsigned long long fault_handler;
	unsigned long long fault_rip;
	int		   faulted;
	unsigned long	   code_size;
	/* resume test: how many times the guest was entered, and what it counted */
	int		   iterations;
	unsigned long long counter;
	unsigned long long reg_accum;
	/* what the fault stubs recorded */
	unsigned long long guest_stubs;
	unsigned long long guest_tss;
	unsigned long long ist_stack;
	unsigned long long guest_text;
	unsigned long long guest_stack;
	unsigned long	   tables;
	unsigned long long vector;
	unsigned long long error_code;
	unsigned long long cr2;
	/* the pre-flight walk of the guest tables, before CR3 was ever loaded */
	int		   preflight_checked;
	int		   preflight_failed;
	int		   preflight_level;
	unsigned long long preflight_va;
} co_arch_switch_test_t;

extern co_rc_t co_arch_test_switch(co_manager_t* manager, co_arch_switch_test_t* out);

/* Enter the guest address space, run code there, and come back. */
extern co_rc_t co_arch_test_roundtrip(co_manager_t* manager, co_arch_switch_test_t* out,
				      int provoke_fault);

/*
 * Enter a guest whose code and stack live outside the passage page, in an
 * address space built a page at a time.
 */
extern co_rc_t co_arch_test_extern_guest(co_manager_t* manager,
					 co_arch_switch_test_t* out,
					 bool_t provoke_fault);

/*
 * Calling functions the loaded kernel compiled. memset is checked by reading
 * back what it wrote, strlen by what it returns -- two different kinds of
 * evidence, so a stub that merely returned plausibly fails one of them.
 */
typedef struct {
	int		   supported;
	int		   succeeded;
	unsigned long long memset_va;
	unsigned long long strlen_va;
	unsigned long long scratch_va;
	unsigned long long memset_ret;
	unsigned long long strlen_ret;
	unsigned long long strlen_expected;
	unsigned long long snprintf_va;
	unsigned long long snprintf_ret;
	unsigned long long snprintf_expected;
	int		   pattern_ok;
	int		   text_ok;
	char		   text[128];
	unsigned long long console_ring_va;
	unsigned long long console_written;
	unsigned long long console_capacity;
	int		   console_ok;
	char		   console_text[160];
	int		   first_bad;
	int		   first_bad_byte;
	int		   faulted;
	unsigned long long vector;
	unsigned long long fault_rip;
	unsigned long long cr2;
} co_arch_kcall_test_t;

struct co_arch_guest_space;
extern co_rc_t co_arch_test_kernel_code(co_manager_t* manager,
					struct co_arch_guest_space* space,
					unsigned long long memset_va,
					unsigned long long strlen_va,
					unsigned long long snprintf_va,
					unsigned long long early_printk_va,
					unsigned long long early_console_va,
					unsigned long long colinux_console_va,
					unsigned long long ring_symbol_va,
					co_arch_kcall_test_t* out);

/*
 * Hand an interrupt taken in the guest to the host handler that owns it.
 * Returns false if the gate cannot be entered this way, which the caller must
 * treat as a reason to stop rather than as a no-op.
 */
extern bool_t co_arch_forward_host_interrupt(void* host_idt, unsigned long long vector);

#define CO_BOOT_TRACE 16	/* power of two: the loop masks with it */

/* Booting the loaded kernel: enter co_arch_start_kernel and see how far it gets. */
typedef struct {
	unsigned long long entry_va;
	unsigned long long initial_code_va;
	unsigned long long start_kernel_va;
	unsigned long long early_console_va;
	unsigned long long colinux_console_va;
	unsigned long long ring_symbol_va;
	unsigned long long guest_flag_va;
	int		   max_switches;
	int		   step;
} co_arch_boot_t;

typedef struct {
	int		   supported;
	unsigned long long entry_va;
	unsigned long long guest_cr3;
	unsigned long	   tables;
	unsigned long long console_ring_va;
	unsigned long long console_written;
	unsigned long long console_capacity;
	char		   console_text[2048];
	int		   faulted;
	int		   returned_voluntarily;
	int		   hit_limit;
	int		   unforwardable;
	int		   vmx_present;
	unsigned long	   switches;
	unsigned long	   interrupts;
	unsigned long	   steps;
	unsigned long	   trace_next;
	unsigned long long trace[CO_BOOT_TRACE];
	unsigned long long vector;
	unsigned long long fault_rip;
	unsigned long long error_code;
	unsigned long long cr2;
	int		   preflight_checked;
	int		   preflight_failed;
	int		   preflight_level;
	unsigned long long preflight_va;
	/*
	 * What the host's own CPU state looked like after the crossing that
	 * broke it.
	 *
	 * The failures that cost the most time were not the ones that faulted.
	 * They were the ones where the guest ran, control came back, and the
	 * machine died some thousands of instructions later -- once in the
	 * middle of two consecutive co_debug() calls that do nothing but log.
	 * A death at an arbitrary later instruction is the signature of host
	 * state that was quietly corrupted and then used, and no amount of
	 * looking at where it died says which register it was.
	 *
	 * So the host checks itself after every single crossing against a
	 * snapshot taken before the first one, and stops at the first
	 * divergence. That turns "it died somewhere" into "step 4481 changed
	 * IDTR", which is an answer.
	 */
	int		   host_corrupt_field;	/* co_host_field_t, 0 = clean */
	unsigned long	   host_corrupt_step;
	unsigned long long host_corrupt_expected;
	unsigned long long host_corrupt_actual;
} co_arch_boot_result_t;

/*
 * The pieces of host CPU state a crossing could plausibly damage: everything
 * the switch saves and restores, plus the ones it does not and therefore might
 * be assuming wrongly. Reported by number so the driver need not format
 * strings; the daemon names them.
 */
typedef enum {
	CO_HOST_FIELD_NONE = 0,
	CO_HOST_FIELD_CPU,
	CO_HOST_FIELD_CR0,
	CO_HOST_FIELD_CR4,
	CO_HOST_FIELD_CR3,
	CO_HOST_FIELD_GDT_BASE,
	CO_HOST_FIELD_GDT_LIMIT,
	CO_HOST_FIELD_IDT_BASE,
	CO_HOST_FIELD_IDT_LIMIT,
	CO_HOST_FIELD_TR,
	CO_HOST_FIELD_FS_BASE,
	CO_HOST_FIELD_GS_BASE,
	CO_HOST_FIELD_KERNEL_GS_BASE,
	CO_HOST_FIELD_LSTAR,
	CO_HOST_FIELD_STAR,
	CO_HOST_FIELD_SFMASK,
	CO_HOST_FIELD_EFER,
	CO_HOST_FIELD_CS,
	CO_HOST_FIELD_SS,
	CO_HOST_FIELD_MAX
} co_host_field_t;

struct co_arch_guest_space;
extern co_rc_t co_arch_boot_loaded(co_manager_t* manager,
				   struct co_arch_guest_space* space,
				   co_arch_boot_t* in, co_arch_boot_result_t* out);

/* Enter an address space that already holds a loaded kernel image. */
struct co_arch_guest_space;
extern co_rc_t co_arch_enter_loaded(co_manager_t* manager,
				    struct co_arch_guest_space* space,
				    unsigned long long entry_va,
				    co_arch_switch_test_t* out);

/* Enter the guest N times and require it to continue from where it stopped. */
extern co_rc_t co_arch_test_resume(co_manager_t* manager, co_arch_switch_test_t* out,
				   int iterations);

#endif
