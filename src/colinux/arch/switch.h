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

/*
 * Ask a running monitor loop to stop, and see whether one still is.
 *
 * Called from driver unload: the loop may be holding a thread inside the
 * driver long after the process that started it was killed, and tearing the
 * driver's state down underneath it is a use-after-free.
 */
extern void co_arch_boot_abort(void);
extern int  co_arch_boot_running(void);

#define CO_BOOT_TRACE 16	/* power of two: the loop masks with it */

/*
 * How long the monitor loop may hold the ioctl thread, in seconds.
 *
 * The runaway backstop, and the only bound that means anything now that
 * replayed host interrupts no longer spend the guest's switch budget. A guest
 * that has finished booting stops on its own at the idle cap; this is for the
 * one that does not stop.
 */
#define CO_BOOT_MAX_SECONDS 120

/*
 * And how long with a terminal attached: no limit at all.
 *
 * Zero means no deadline. A guest with a console is one somebody is using, and
 * ending it on a timer is wrong twice over -- it interrupts a session that was
 * working, and it does it during whatever was running at the time, which for a
 * package install is the worst possible moment. It was 900 seconds, and a
 * pacman run outlived it.
 *
 * What makes removing it safe is that the deadline is no longer the only way to
 * end a run. CO_MANAGER_IOCTL_KSTOP asks the loop to stop from another process
 * and it answers within a crossing (stop.bat), and driver unload does the same
 * and waits. The headless bound below stays, because a bring-up run with
 * nobody attached still needs to stop by itself and report.
 */
#define CO_BOOT_CONSOLE_SECONDS 0

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
	/*
	 * Instructions the guest's own trap stub may step through before it
	 * has to hand the processor back. 0 keeps the old behaviour of one
	 * world switch per instruction.
	 */
	int		   batch;
	/* the kernel's own page tables, init_top_pgt first; see kload.h */
	unsigned long long kernel_tables[8];
	int		   kernel_table_count;
	/* the guest's __ex_table, which the host applies on its behalf */
	unsigned long long ex_table_start;
	unsigned long long ex_table_stop;
	/*
	 * Where the guest keeps co_colinux_passage_page. The host writes the
	 * passage page's address there before entry, and the guest yields
	 * cooperatively by calling the switch blob through it.
	 */
	unsigned long long passage_symbol_va;
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
	int		   hit_deadline;
	unsigned long	   guest_switches;
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
	/*
	 * The guest's rdi and rax when it faulted.
	 *
	 * The stub already pushes all fifteen registers onto the IST stack, so
	 * this costs a read. It is the difference between "the CPU refused a
	 * CR4 write" and knowing which bit it refused -- and a fault whose
	 * operand is invisible is a fault that gets guessed at.
	 */
	unsigned long long fault_rdi;
	unsigned long long fault_rax;
	/*
	 * The whole register frame at the last stop, fault or limit.
	 *
	 * fault_rdi/rax name two registers, which is enough for a fault whose
	 * operand is known. A hang is different: the run stops on the switch
	 * budget somewhere inside a loop, and which loop is a question the
	 * instruction pointer answers but why it does not exit is a question the
	 * registers answer -- a list iterator that never reaches its sentinel, a
	 * scan pointer that ran off the end of a string. In the stub's frame
	 * order: [0] r15, then r14 r13 r12 r11 r10 r9 r8 rbp rdi rsi rdx rcx
	 * rbx, [14] rax.
	 */
	unsigned long long stop_regs[15];
	/*
	 * WARN_ON and friends compile to ud2 with an entry in __bug_table. On
	 * real hardware the kernel's own #UD handler finds that entry, prints
	 * the warning and steps over the instruction. A cooperative guest keeps
	 * the host's IDT, so that handler never runs and the host has to do it
	 * -- otherwise a warning, which is a log line, stops the run.
	 */
	unsigned long	   warnings;
	unsigned long long warning_rip[8];
	/* faults recovered from the guest's own __ex_table */
	unsigned long	   fixups;
	unsigned long long fixup_rip[8];
	int		   fixup_type[8];
	int		   fault_extype;	/* set if the fatal fault had an entry */
	int		   reached_idle;	/* guest halted -- booted through to idle */
	/*
	 * The cooperative protocol. run_yields are active safe-point crossings;
	 * idle_yields are CO_OPERATION_IDLE crossings. terminated and
	 * terminate_reason report a guest-requested shutdown; stop_operation is
	 * whatever unhandled operation ended the run.
	 */
	unsigned long	   run_yields;
	unsigned long	   idle_yields;
	/*
	 * Cooperative block I/O. block_errors counts transfers the host
	 * refused or the backing store failed; the guest is told about each
	 * one and turns it into an I/O error, so a run that mounts a
	 * filesystem successfully but reports errors here has been reading
	 * something it should not have.
	 */
	unsigned long	   block_requests;
	unsigned long	   block_errors;
	int		   terminated;
	unsigned long long terminate_reason;
	unsigned long long stop_operation;
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
	CO_HOST_FIELD_DS,
	CO_HOST_FIELD_ES,
	CO_HOST_FIELD_FS,
	CO_HOST_FIELD_GS,
	CO_HOST_FIELD_CR8,
	CO_HOST_FIELD_PAT,
	CO_HOST_FIELD_DR7,
	CO_HOST_FIELD_RFLAGS,
	CO_HOST_FIELD_CR2,
	CO_HOST_FIELD_LDT,
	CO_HOST_FIELD_CSTAR,
	CO_HOST_FIELD_SYSENTER_CS,
	CO_HOST_FIELD_SYSENTER_ESP,
	CO_HOST_FIELD_SYSENTER_EIP,
	CO_HOST_FIELD_DR0,
	CO_HOST_FIELD_DR1,
	CO_HOST_FIELD_DR2,
	CO_HOST_FIELD_DR3,
	CO_HOST_FIELD_DR6,
	CO_HOST_FIELD_XCR0,
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
