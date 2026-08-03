// SPDX-License-Identifier: GPL-2.0
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/smp.h>
#include <linux/cpu.h>
#include <linux/prctl.h>
#include <linux/slab.h>
#include <linux/sched.h>
#include <linux/sched/idle.h>
#include <linux/sched/debug.h>
#include <linux/sched/task.h>
#include <linux/sched/task_stack.h>
#include <linux/init.h>
#include <linux/export.h>
#include <linux/pm.h>
#include <linux/tick.h>
#include <linux/random.h>
#include <linux/user-return-notifier.h>
#include <linux/dmi.h>
#include <linux/utsname.h>
#include <linux/stackprotector.h>
#include <linux/cpuidle.h>
#include <linux/acpi.h>
#include <linux/elf-randomize.h>
#include <linux/static_call.h>
#include <trace/events/power.h>
#include <linux/hw_breakpoint.h>
#include <linux/entry-common.h>
#include <asm/cpu.h>
#include <asm/cpuid/api.h>
#include <asm/apic.h>
#include <linux/uaccess.h>
#include <asm/mwait.h>
#include <asm/fpu/api.h>
#include <asm/fpu/sched.h>
#include <asm/fpu/xstate.h>
#include <asm/debugreg.h>
#include <asm/nmi.h>
#include <asm/tlbflush.h>
#include <asm/mce.h>
#include <asm/vm86.h>
#include <asm/switch_to.h>
#include <asm/desc.h>
#include <asm/prctl.h>
#include <asm/spec-ctrl.h>
#include <asm/io_bitmap.h>
#include <asm/proto.h>
#include <asm/frame.h>
#include <asm/unwind.h>
/* DEFINE_IDTENTRY_SYSVEC, for the cooperative timer entry below. */
#include <asm/idtentry.h>
#include <asm/tdx.h>
#include <asm/mmu_context.h>
#include <asm/msr.h>
#include <asm/shstk.h>
#include <linux/console.h>	/* co_colinux_guest */
#include <linux/clockchips.h>	/* the cooperative tick device */
#include <linux/hardirq.h>	/* irq_enter/irq_exit around delivered ticks */
#include <asm/irq_regs.h>	/* the register frame the tick handler reads */

#include "process.h"

/*
 * per-CPU TSS segments. Threads are completely 'soft' on Linux,
 * no more per-task TSS's. The TSS size is kept cacheline-aligned
 * so they are allowed to end up in the .data..cacheline_aligned
 * section. Since TSS's are completely CPU-local, we want them
 * on exact cacheline boundaries, to eliminate cacheline ping-pong.
 */
__visible DEFINE_PER_CPU_PAGE_ALIGNED(struct tss_struct, cpu_tss_rw) = {
	.x86_tss = {
		/*
		 * .sp0 is only used when entering ring 0 from a lower
		 * privilege level.  Since the init task never runs anything
		 * but ring 0 code, there is no need for a valid value here.
		 * Poison it.
		 */
		.sp0 = (1UL << (BITS_PER_LONG-1)) + 1,

#ifdef CONFIG_X86_32
		.sp1 = TOP_OF_INIT_STACK,

		.ss0 = __KERNEL_DS,
		.ss1 = __KERNEL_CS,
#endif
		.io_bitmap_base	= IO_BITMAP_OFFSET_INVALID,
	 },
};
EXPORT_PER_CPU_SYMBOL(cpu_tss_rw);

DEFINE_PER_CPU(bool, __tss_limit_invalid);
EXPORT_PER_CPU_SYMBOL_GPL(__tss_limit_invalid);

/*
 * The cache may be in an incoherent state and needs flushing during kexec.
 * E.g., on SME/TDX platforms, dirty cacheline aliases with and without
 * encryption bit(s) can coexist and the cache needs to be flushed before
 * booting to the new kernel to avoid the silent memory corruption due to
 * dirty cachelines with different encryption property being written back
 * to the memory.
 */
DEFINE_PER_CPU(bool, cache_state_incoherent);

/*
 * this gets called so that we can store lazy state into memory and copy the
 * current task into the new thread.
 */
int arch_dup_task_struct(struct task_struct *dst, struct task_struct *src)
{
	/* fpu_clone() will initialize the "dst_fpu" memory */
	memcpy_and_pad(dst, arch_task_struct_size, src, sizeof(*dst), 0);

#ifdef CONFIG_VM86
	dst->thread.vm86 = NULL;
#endif

	return 0;
}

#ifdef CONFIG_X86_64
void arch_release_task_struct(struct task_struct *tsk)
{
	if (fpu_state_size_dynamic() && !(tsk->flags & (PF_KTHREAD | PF_USER_WORKER)))
		fpstate_free(x86_task_fpu(tsk));
}
#endif

/*
 * Free thread data structures etc..
 */
void exit_thread(struct task_struct *tsk)
{
	struct thread_struct *t = &tsk->thread;

	if (test_thread_flag(TIF_IO_BITMAP))
		io_bitmap_exit(tsk);

	free_vm86(t);

	shstk_free(tsk);
	fpu__drop(tsk);
}

static int set_new_tls(struct task_struct *p, unsigned long tls)
{
	struct user_desc __user *utls = (struct user_desc __user *)tls;

	if (in_ia32_syscall())
		return do_set_thread_area(p, -1, utls, 0);
	else
		return do_set_thread_area_64(p, ARCH_SET_FS, tls);
}

__visible void ret_from_fork(struct task_struct *prev, struct pt_regs *regs,
				     int (*fn)(void *), void *fn_arg)
{
	schedule_tail(prev);

	/* Is this a kernel thread? */
	if (unlikely(fn)) {
		fn(fn_arg);
		/*
		 * A kernel thread is allowed to return here after successfully
		 * calling kernel_execve().  Exit to userspace to complete the
		 * execve() syscall.
		 */
		regs->ax = 0;
	}

	syscall_exit_to_user_mode(regs);
}

int copy_thread(struct task_struct *p, const struct kernel_clone_args *args)
{
	u64 clone_flags = args->flags;
	unsigned long sp = args->stack;
	unsigned long tls = args->tls;
	struct inactive_task_frame *frame;
	struct fork_frame *fork_frame;
	struct pt_regs *childregs;
	unsigned long new_ssp;
	int ret = 0;

	childregs = task_pt_regs(p);
	fork_frame = container_of(childregs, struct fork_frame, regs);
	frame = &fork_frame->frame;

	frame->bp = encode_frame_pointer(childregs);
	frame->ret_addr = (unsigned long) ret_from_fork_asm;
	p->thread.sp = (unsigned long) fork_frame;
	p->thread.io_bitmap = NULL;
	clear_tsk_thread_flag(p, TIF_IO_BITMAP);
	p->thread.iopl_warn = 0;
	memset(p->thread.ptrace_bps, 0, sizeof(p->thread.ptrace_bps));

#ifdef CONFIG_X86_64
	current_save_fsgs();
	p->thread.fsindex = current->thread.fsindex;
	p->thread.fsbase = current->thread.fsbase;
	p->thread.gsindex = current->thread.gsindex;
	p->thread.gsbase = current->thread.gsbase;

	savesegment(es, p->thread.es);
	savesegment(ds, p->thread.ds);

	if (p->mm && (clone_flags & (CLONE_VM | CLONE_VFORK)) == CLONE_VM)
		set_bit(MM_CONTEXT_LOCK_LAM, &p->mm->context.flags);
#else
	p->thread.sp0 = (unsigned long) (childregs + 1);
	savesegment(gs, p->thread.gs);
	/*
	 * Clear all status flags including IF and set fixed bit. 64bit
	 * does not have this initialization as the frame does not contain
	 * flags. The flags consistency (especially vs. AC) is there
	 * ensured via objtool, which lacks 32bit support.
	 */
	frame->flags = X86_EFLAGS_FIXED;
#endif

	/*
	 * Allocate a new shadow stack for thread if needed. If shadow stack,
	 * is disabled, new_ssp will remain 0, and fpu_clone() will know not to
	 * update it.
	 */
	new_ssp = shstk_alloc_thread_stack(p, clone_flags, args->stack_size);
	if (IS_ERR_VALUE(new_ssp))
		return PTR_ERR((void *)new_ssp);

	fpu_clone(p, clone_flags, args->fn, new_ssp);

	/* Kernel thread ? */
	if (unlikely(p->flags & PF_KTHREAD)) {
		p->thread.pkru = pkru_get_init_value();
		memset(childregs, 0, sizeof(struct pt_regs));
		kthread_frame_init(frame, args->fn, args->fn_arg);
		return 0;
	}

	/*
	 * Clone current's PKRU value from hardware. tsk->thread.pkru
	 * is only valid when scheduled out.
	 */
	p->thread.pkru = read_pkru();

	frame->bx = 0;
	*childregs = *current_pt_regs();
	childregs->ax = 0;
	if (sp)
		childregs->sp = sp;

	if (unlikely(args->fn)) {
		/*
		 * A user space thread, but it doesn't return to
		 * ret_after_fork().
		 *
		 * In order to indicate that to tools like gdb,
		 * we reset the stack and instruction pointers.
		 *
		 * It does the same kernel frame setup to return to a kernel
		 * function that a kernel thread does.
		 */
		childregs->sp = 0;
		childregs->ip = 0;
		kthread_frame_init(frame, args->fn, args->fn_arg);
		return 0;
	}

	/* Set a new TLS for the child thread? */
	if (clone_flags & CLONE_SETTLS)
		ret = set_new_tls(p, tls);

	if (!ret && unlikely(test_tsk_thread_flag(current, TIF_IO_BITMAP)))
		io_bitmap_share(p);

	return ret;
}

static void pkru_flush_thread(void)
{
	/*
	 * If PKRU is enabled the default PKRU value has to be loaded into
	 * the hardware right here (similar to context switch).
	 */
	pkru_write_default();
}

void flush_thread(void)
{
	struct task_struct *tsk = current;

	flush_ptrace_hw_breakpoint(tsk);
	memset(tsk->thread.tls_array, 0, sizeof(tsk->thread.tls_array));

	fpu_flush_thread();
	pkru_flush_thread();
}

void disable_TSC(void)
{
	preempt_disable();
	if (!test_and_set_thread_flag(TIF_NOTSC))
		/*
		 * Must flip the CPU state synchronously with
		 * TIF_NOTSC in the current running context.
		 */
		cr4_set_bits(X86_CR4_TSD);
	preempt_enable();
}

static void enable_TSC(void)
{
	preempt_disable();
	if (test_and_clear_thread_flag(TIF_NOTSC))
		/*
		 * Must flip the CPU state synchronously with
		 * TIF_NOTSC in the current running context.
		 */
		cr4_clear_bits(X86_CR4_TSD);
	preempt_enable();
}

int get_tsc_mode(unsigned long adr)
{
	unsigned int val;

	if (test_thread_flag(TIF_NOTSC))
		val = PR_TSC_SIGSEGV;
	else
		val = PR_TSC_ENABLE;

	return put_user(val, (unsigned int __user *)adr);
}

int set_tsc_mode(unsigned int val)
{
	if (val == PR_TSC_SIGSEGV)
		disable_TSC();
	else if (val == PR_TSC_ENABLE)
		enable_TSC();
	else
		return -EINVAL;

	return 0;
}

DEFINE_PER_CPU(u64, msr_misc_features_shadow);

static void set_cpuid_faulting(bool on)
{

	if (boot_cpu_data.x86_vendor == X86_VENDOR_INTEL) {
		u64 msrval;

		msrval = this_cpu_read(msr_misc_features_shadow);
		msrval &= ~MSR_MISC_FEATURES_ENABLES_CPUID_FAULT;
		msrval |= (on << MSR_MISC_FEATURES_ENABLES_CPUID_FAULT_BIT);
		this_cpu_write(msr_misc_features_shadow, msrval);
		wrmsrq(MSR_MISC_FEATURES_ENABLES, msrval);
	} else if (boot_cpu_data.x86_vendor == X86_VENDOR_AMD) {
		if (on)
			msr_set_bit(MSR_K7_HWCR, MSR_K7_HWCR_CPUID_USER_DIS_BIT);
		else
			msr_clear_bit(MSR_K7_HWCR, MSR_K7_HWCR_CPUID_USER_DIS_BIT);
	}
}

static void disable_cpuid(void)
{
	preempt_disable();
	if (!test_and_set_thread_flag(TIF_NOCPUID)) {
		/*
		 * Must flip the CPU state synchronously with
		 * TIF_NOCPUID in the current running context.
		 */
		set_cpuid_faulting(true);
	}
	preempt_enable();
}

static void enable_cpuid(void)
{
	preempt_disable();
	if (test_and_clear_thread_flag(TIF_NOCPUID)) {
		/*
		 * Must flip the CPU state synchronously with
		 * TIF_NOCPUID in the current running context.
		 */
		set_cpuid_faulting(false);
	}
	preempt_enable();
}

static int get_cpuid_mode(void)
{
	return !test_thread_flag(TIF_NOCPUID);
}

static int set_cpuid_mode(unsigned long cpuid_enabled)
{
	if (!boot_cpu_has(X86_FEATURE_CPUID_FAULT))
		return -ENODEV;

	if (cpuid_enabled)
		enable_cpuid();
	else
		disable_cpuid();

	return 0;
}

/*
 * Called immediately after a successful exec.
 */
void arch_setup_new_exec(void)
{
	/* If cpuid was previously disabled for this task, re-enable it. */
	if (test_thread_flag(TIF_NOCPUID))
		enable_cpuid();

	/*
	 * Don't inherit TIF_SSBD across exec boundary when
	 * PR_SPEC_DISABLE_NOEXEC is used.
	 */
	if (test_thread_flag(TIF_SSBD) &&
	    task_spec_ssb_noexec(current)) {
		clear_thread_flag(TIF_SSBD);
		task_clear_spec_ssb_disable(current);
		task_clear_spec_ssb_noexec(current);
		speculation_ctrl_update(read_thread_flags());
	}

	mm_reset_untag_mask(current->mm);
}

#ifdef CONFIG_X86_IOPL_IOPERM
static inline void switch_to_bitmap(unsigned long tifp)
{
	/*
	 * Invalidate I/O bitmap if the previous task used it. This prevents
	 * any possible leakage of an active I/O bitmap.
	 *
	 * If the next task has an I/O bitmap it will handle it on exit to
	 * user mode.
	 */
	if (tifp & _TIF_IO_BITMAP)
		tss_invalidate_io_bitmap();
}

static void tss_copy_io_bitmap(struct tss_struct *tss, struct io_bitmap *iobm)
{
	/*
	 * Copy at least the byte range of the incoming tasks bitmap which
	 * covers the permitted I/O ports.
	 *
	 * If the previous task which used an I/O bitmap had more bits
	 * permitted, then the copy needs to cover those as well so they
	 * get turned off.
	 */
	memcpy(tss->io_bitmap.bitmap, iobm->bitmap,
	       max(tss->io_bitmap.prev_max, iobm->max));

	/*
	 * Store the new max and the sequence number of this bitmap
	 * and a pointer to the bitmap itself.
	 */
	tss->io_bitmap.prev_max = iobm->max;
	tss->io_bitmap.prev_sequence = iobm->sequence;
}

/**
 * native_tss_update_io_bitmap - Update I/O bitmap before exiting to user mode
 */
void native_tss_update_io_bitmap(void)
{
	struct tss_struct *tss = this_cpu_ptr(&cpu_tss_rw);
	struct thread_struct *t = &current->thread;
	u16 *base = &tss->x86_tss.io_bitmap_base;

	if (!test_thread_flag(TIF_IO_BITMAP)) {
		native_tss_invalidate_io_bitmap();
		return;
	}

	if (IS_ENABLED(CONFIG_X86_IOPL_IOPERM) && t->iopl_emul == 3) {
		*base = IO_BITMAP_OFFSET_VALID_ALL;
	} else {
		struct io_bitmap *iobm = t->io_bitmap;

		if (WARN_ON_ONCE(!iobm)) {
			clear_thread_flag(TIF_IO_BITMAP);
			native_tss_invalidate_io_bitmap();
		}

		/*
		 * Only copy bitmap data when the sequence number differs. The
		 * update time is accounted to the incoming task.
		 */
		if (tss->io_bitmap.prev_sequence != iobm->sequence)
			tss_copy_io_bitmap(tss, iobm);

		/* Enable the bitmap */
		*base = IO_BITMAP_OFFSET_VALID_MAP;
	}

	/*
	 * Make sure that the TSS limit is covering the IO bitmap. It might have
	 * been cut down by a VMEXIT to 0x67 which would cause a subsequent I/O
	 * access from user space to trigger a #GP because the bitmap is outside
	 * the TSS limit.
	 */
	refresh_tss_limit();
}
#else /* CONFIG_X86_IOPL_IOPERM */
static inline void switch_to_bitmap(unsigned long tifp) { }
#endif

#ifdef CONFIG_SMP

struct ssb_state {
	struct ssb_state	*shared_state;
	raw_spinlock_t		lock;
	unsigned int		disable_state;
	unsigned long		local_state;
};

#define LSTATE_SSB	0

static DEFINE_PER_CPU(struct ssb_state, ssb_state);

void speculative_store_bypass_ht_init(void)
{
	struct ssb_state *st = this_cpu_ptr(&ssb_state);
	unsigned int this_cpu = smp_processor_id();
	unsigned int cpu;

	st->local_state = 0;

	/*
	 * Shared state setup happens once on the first bringup
	 * of the CPU. It's not destroyed on CPU hotunplug.
	 */
	if (st->shared_state)
		return;

	raw_spin_lock_init(&st->lock);

	/*
	 * Go over HT siblings and check whether one of them has set up the
	 * shared state pointer already.
	 */
	for_each_cpu(cpu, topology_sibling_cpumask(this_cpu)) {
		if (cpu == this_cpu)
			continue;

		if (!per_cpu(ssb_state, cpu).shared_state)
			continue;

		/* Link it to the state of the sibling: */
		st->shared_state = per_cpu(ssb_state, cpu).shared_state;
		return;
	}

	/*
	 * First HT sibling to come up on the core.  Link shared state of
	 * the first HT sibling to itself. The siblings on the same core
	 * which come up later will see the shared state pointer and link
	 * themselves to the state of this CPU.
	 */
	st->shared_state = st;
}

/*
 * Logic is: First HT sibling enables SSBD for both siblings in the core
 * and last sibling to disable it, disables it for the whole core. This how
 * MSR_SPEC_CTRL works in "hardware":
 *
 *  CORE_SPEC_CTRL = THREAD0_SPEC_CTRL | THREAD1_SPEC_CTRL
 */
static __always_inline void amd_set_core_ssb_state(unsigned long tifn)
{
	struct ssb_state *st = this_cpu_ptr(&ssb_state);
	u64 msr = x86_amd_ls_cfg_base;

	if (!static_cpu_has(X86_FEATURE_ZEN)) {
		msr |= ssbd_tif_to_amd_ls_cfg(tifn);
		wrmsrq(MSR_AMD64_LS_CFG, msr);
		return;
	}

	if (tifn & _TIF_SSBD) {
		/*
		 * Since this can race with prctl(), block reentry on the
		 * same CPU.
		 */
		if (__test_and_set_bit(LSTATE_SSB, &st->local_state))
			return;

		msr |= x86_amd_ls_cfg_ssbd_mask;

		raw_spin_lock(&st->shared_state->lock);
		/* First sibling enables SSBD: */
		if (!st->shared_state->disable_state)
			wrmsrq(MSR_AMD64_LS_CFG, msr);
		st->shared_state->disable_state++;
		raw_spin_unlock(&st->shared_state->lock);
	} else {
		if (!__test_and_clear_bit(LSTATE_SSB, &st->local_state))
			return;

		raw_spin_lock(&st->shared_state->lock);
		st->shared_state->disable_state--;
		if (!st->shared_state->disable_state)
			wrmsrq(MSR_AMD64_LS_CFG, msr);
		raw_spin_unlock(&st->shared_state->lock);
	}
}
#else
static __always_inline void amd_set_core_ssb_state(unsigned long tifn)
{
	u64 msr = x86_amd_ls_cfg_base | ssbd_tif_to_amd_ls_cfg(tifn);

	wrmsrq(MSR_AMD64_LS_CFG, msr);
}
#endif

static __always_inline void amd_set_ssb_virt_state(unsigned long tifn)
{
	/*
	 * SSBD has the same definition in SPEC_CTRL and VIRT_SPEC_CTRL,
	 * so ssbd_tif_to_spec_ctrl() just works.
	 */
	wrmsrq(MSR_AMD64_VIRT_SPEC_CTRL, ssbd_tif_to_spec_ctrl(tifn));
}

/*
 * Update the MSRs managing speculation control, during context switch.
 *
 * tifp: Previous task's thread flags
 * tifn: Next task's thread flags
 */
static __always_inline void __speculation_ctrl_update(unsigned long tifp,
						      unsigned long tifn)
{
	unsigned long tif_diff = tifp ^ tifn;
	u64 msr = x86_spec_ctrl_base;
	bool updmsr = false;

	lockdep_assert_irqs_disabled();

	/* Handle change of TIF_SSBD depending on the mitigation method. */
	if (static_cpu_has(X86_FEATURE_VIRT_SSBD)) {
		if (tif_diff & _TIF_SSBD)
			amd_set_ssb_virt_state(tifn);
	} else if (static_cpu_has(X86_FEATURE_LS_CFG_SSBD)) {
		if (tif_diff & _TIF_SSBD)
			amd_set_core_ssb_state(tifn);
	} else if (static_cpu_has(X86_FEATURE_SPEC_CTRL_SSBD) ||
		   static_cpu_has(X86_FEATURE_AMD_SSBD)) {
		updmsr |= !!(tif_diff & _TIF_SSBD);
		msr |= ssbd_tif_to_spec_ctrl(tifn);
	}

	/* Only evaluate TIF_SPEC_IB if conditional STIBP is enabled. */
	if (IS_ENABLED(CONFIG_SMP) &&
	    static_branch_unlikely(&switch_to_cond_stibp)) {
		updmsr |= !!(tif_diff & _TIF_SPEC_IB);
		msr |= stibp_tif_to_spec_ctrl(tifn);
	}

	if (updmsr)
		update_spec_ctrl_cond(msr);
}

static unsigned long speculation_ctrl_update_tif(struct task_struct *tsk)
{
	if (test_and_clear_tsk_thread_flag(tsk, TIF_SPEC_FORCE_UPDATE)) {
		if (task_spec_ssb_disable(tsk))
			set_tsk_thread_flag(tsk, TIF_SSBD);
		else
			clear_tsk_thread_flag(tsk, TIF_SSBD);

		if (task_spec_ib_disable(tsk))
			set_tsk_thread_flag(tsk, TIF_SPEC_IB);
		else
			clear_tsk_thread_flag(tsk, TIF_SPEC_IB);
	}
	/* Return the updated threadinfo flags*/
	return read_task_thread_flags(tsk);
}

void speculation_ctrl_update(unsigned long tif)
{
	unsigned long flags;

	/* Forced update. Make sure all relevant TIF flags are different */
	local_irq_save(flags);
	__speculation_ctrl_update(~tif, tif);
	local_irq_restore(flags);
}

/* Called from seccomp/prctl update */
void speculation_ctrl_update_current(void)
{
	preempt_disable();
	speculation_ctrl_update(speculation_ctrl_update_tif(current));
	preempt_enable();
}

static inline void cr4_toggle_bits_irqsoff(unsigned long mask)
{
	unsigned long newval, cr4 = this_cpu_read(cpu_tlbstate.cr4);

	newval = cr4 ^ mask;
	if (newval != cr4) {
		this_cpu_write(cpu_tlbstate.cr4, newval);
		__write_cr4(newval);
	}
}

void __switch_to_xtra(struct task_struct *prev_p, struct task_struct *next_p)
{
	unsigned long tifp, tifn;

	tifn = read_task_thread_flags(next_p);
	tifp = read_task_thread_flags(prev_p);

	switch_to_bitmap(tifp);

	propagate_user_return_notify(prev_p, next_p);

	if ((tifp & _TIF_BLOCKSTEP || tifn & _TIF_BLOCKSTEP) &&
	    arch_has_block_step()) {
		unsigned long debugctl, msk;

		rdmsrq(MSR_IA32_DEBUGCTLMSR, debugctl);
		debugctl &= ~DEBUGCTLMSR_BTF;
		msk = tifn & _TIF_BLOCKSTEP;
		debugctl |= (msk >> TIF_BLOCKSTEP) << DEBUGCTLMSR_BTF_SHIFT;
		wrmsrq(MSR_IA32_DEBUGCTLMSR, debugctl);
	}

	if ((tifp ^ tifn) & _TIF_NOTSC)
		cr4_toggle_bits_irqsoff(X86_CR4_TSD);

	if ((tifp ^ tifn) & _TIF_NOCPUID)
		set_cpuid_faulting(!!(tifn & _TIF_NOCPUID));

	if (likely(!((tifp | tifn) & _TIF_SPEC_FORCE_UPDATE))) {
		__speculation_ctrl_update(tifp, tifn);
	} else {
		speculation_ctrl_update_tif(prev_p);
		tifn = speculation_ctrl_update_tif(next_p);

		/* Enforce MSR update to ensure consistent state */
		__speculation_ctrl_update(~tifn, tifn);
	}
}

/*
 * Idle related variables and functions
 */
unsigned long boot_option_idle_override = IDLE_NO_OVERRIDE;
EXPORT_SYMBOL(boot_option_idle_override);

/*
 * We use this if we don't have any better idle routine..
 */
void __cpuidle default_idle(void)
{
	raw_safe_halt();
	raw_local_irq_disable();
}
#if defined(CONFIG_APM_MODULE) || defined(CONFIG_HALTPOLL_CPUIDLE_MODULE)
EXPORT_SYMBOL(default_idle);
#endif

DEFINE_STATIC_CALL_NULL(x86_idle, default_idle);

/*
 * Cooperative idle, for coLinux.
 *
 * The passage page, at the address the host primes here before the guest runs,
 * carries the world switch blob and both saved-state blocks. Yielding is a call
 * into that blob with rcx = the guest state (leaving) and rdx = the host state
 * (entering) -- the Microsoft x64 convention the blob is built with, since it is
 * assembled inside the Windows-built driver. The call returns when the host
 * switches back in.
 */
unsigned long long co_colinux_passage_page;
EXPORT_SYMBOL(co_colinux_passage_page);

/*
 * Offsets into co_arch_passage_page_t. These mirror the host's CO_PP_* and the
 * shared struct in the coLinux tree's asm/cooperative.h; the host asserts them
 * against __builtin_offsetof, so a drift is a build error there rather than a
 * silent wrong pointer here. code sits after the seven debug-register slots.
 */
#define CO_PP_CODE		0x38
#define CO_PP_HOST_STATE	0x740
#define CO_PP_LINUXVM_STATE	0x880
#define CO_PP_OPERATION		0x9c0
#define CO_PP_PARAMS		0x9c8	/* params[0]: 0xb50 - 49*8, see below */
#define CO_PP_HOST_MONO		0xb50	/* params[49]: host monotonic clock, 100 ns units */
#define CO_PP_HOST_WALL		0xb58	/* params[50]: host wall clock, UTC time_t seconds */

/* co_operation_t values, from include/linux/cooperative.h in the coLinux tree */
#define CO_OPERATION_IDLE	2
#define CO_OPERATION_TERMINATE	3
#define CO_OPERATION_YIELD	16

static __always_inline void co_colinux_switch(unsigned long long operation)
{
	unsigned char *pp = (unsigned char *)co_colinux_passage_page;
	register void *leaving  asm("rcx") = pp + CO_PP_LINUXVM_STATE;
	register void *entering asm("rdx") = pp + CO_PP_HOST_STATE;
	void *code = pp + CO_PP_CODE;
	*(volatile unsigned long long *)(pp + CO_PP_OPERATION) = operation;

	/*
	 * The crossing itself is made uninterruptible inside the switch (the
	 * cli/sti pair around the CR3 write in arch/x86_64/switch.c), and
	 * deliberately not here.
	 *
	 * Wrapping this call was the obvious-looking fix and was wrong. The
	 * call does not merely cross: the host runs a whole monitor-loop
	 * iteration inside it and re-enters the guest before it returns. So a
	 * cli here is held across all of that, and any path that leaves the
	 * switch without coming back through the matching popfq -- a resume
	 * after a fault, an interrupt replayed and the guest restarted at its
	 * saved frame -- puts the guest back on the processor with real
	 * interrupts off. The guest then runs free and deaf until something
	 * happens to call local_irq_enable(), and if that stretch is long
	 * enough the host misses its clock and the machine freezes. Delivering
	 * a signal and respawning a shell is long enough.
	 *
	 * The window that actually needs covering is five instructions wide
	 * and lives on the other side of this call, which is where it is now
	 * closed.
	 */
	asm volatile(
		"call *%[code]\n"
		: "+r"(leaving), "+r"(entering)
		: [code] "r"(code)
		: "rax", "rsi", "rdi", "r8", "r9", "r10", "r11", "memory", "cc");

	/*
	 * Real interrupts back on, unconditionally, because the guest must
	 * always run with them on and nothing else here will do it.
	 *
	 * The switch clears IF for the crossing and does not restore it -- it
	 * has nowhere to keep the value at that point -- so control arrives
	 * back here deaf. That was already true before the crossing was made
	 * uninterruptible, because the monitor loop clears IF before it enters
	 * the guest; what saved it was that Linux calls local_irq_enable()
	 * often, and that now puts the hardware flag back too (asm/irqflags.h).
	 * Relying on that is relying on a coincidence of timing: the window
	 * between here and the next enable is a window in which the host is not
	 * answering its clock.
	 *
	 * This is the real flag only. The guest's logical interrupt state is
	 * the virtual one and is not touched: code that yielded from inside a
	 * local_irq_disable() section still believes interrupts are off, and
	 * still has them off as far as anything in the guest can observe.
	 */
	asm volatile("sti" : : : "memory");
}

/*
 * Tell the host the guest is finished, and why.
 *
 * A cooperative guest cannot power anything off: there is no hardware to write
 * to, and the processor belongs to a Windows kernel that is still using it.
 * What "shutting down" means here is telling the host that this guest is done
 * so that it can end the monitor loop, free the guest's memory and close the
 * files backing its disks -- which is the whole of the shutdown that exists to
 * be performed.
 *
 * This is the cooperative protocol's own message, and it replaces a signature
 * the host had to recognise by sniffing opcodes. native_halt() emits ud2; ud2
 * and the host reads the instruction stream to tell that apart from a WARN's
 * single ud2. That works, and it is a reasonable thing to keep for the paths
 * where nothing better is possible -- a panic, a BUG, anything that arrives at
 * halt without having planned to. It is the wrong way to say "the operator
 * typed poweroff", which is an orderly event the guest can announce properly:
 * with a reason, after the filesystems have been flushed and unmounted, at a
 * point where the host can distinguish a clean exit from a crash.
 *
 * Does not return. If the host re-enters us anyway, spin on the halt so we do
 * not run on with the state teardown already done.
 */
void co_colinux_terminate(unsigned long reason)
{
	unsigned char *pp = (unsigned char *)co_colinux_passage_page;

	if (pp) {
		*(volatile unsigned long long *)(pp + CO_PP_PARAMS) = reason;
		co_colinux_switch(CO_OPERATION_TERMINATE);
	}

	for (;;)
		native_halt();
}
EXPORT_SYMBOL(co_colinux_terminate);

/*
 * Give the processor back at a safe guest boundary. This is intentionally a
 * real out-of-line, non-instrumented function: native_irq_enable() calls it
 * from throughout the kernel and tracing the boundary could recurse back into
 * the same irqflag path.
 */
noinstr void co_colinux_yield(void)
{
	if (co_colinux_passage_page)
		co_colinux_switch(CO_OPERATION_YIELD);
}

/*
 * Virtual time: the host's clock, delivered as ticks.
 *
 * The guest has no timer hardware -- nolapic, nohpet, no PIT it may touch --
 * so nothing periodic ever fires on its own and jiffies would stand still
 * forever (the first casualty being calibrate_delay, which spins on jiffies
 * advancing). The host, meanwhile, has a perfectly good clock, and stamps its
 * monotonic time into the passage page before every re-entry.
 *
 * This is coLinux's co_handle_jiffies one architecture over: a clock_event
 * device whose events are synthesised from the host's clock at cooperative
 * boundaries rather than raised by an interrupt line. The device is
 * periodic-only, which keeps the tick core in periodic mode -- no oneshot, no
 * NO_HZ engagement, one code path to trust during bring-up.
 *
 * Delivery happens at the idle boundary: co_cpu_idle drains the elapsed host
 * time as N calls of the tick handler, each inside irq_enter/irq_exit exactly
 * as a hardware tick would be. While the guest is busy computing, time is not
 * delivered -- it catches up, clamped, at the next idle. That is the honest
 * cooperative trade until interrupt-path delivery (the COPIC) exists.
 */
static int co_colinux_ce_state_noop(struct clock_event_device *evt)
{
	return 0;
}

static struct clock_event_device co_colinux_clockevent = {
	.name			= "cotimer",
	.features		= CLOCK_EVT_FEAT_PERIODIC,
	.set_state_periodic	= co_colinux_ce_state_noop,
	.set_state_shutdown	= co_colinux_ce_state_noop,
	.rating			= 300,
};

void __init co_colinux_timer_init(void)
{
	co_colinux_clockevent.cpumask = cpumask_of(0);
	clockevents_register_device(&co_colinux_clockevent);
	pr_info("cotimer: cooperative tick device registered, HZ=%d\n", HZ);
}

/*
 * How many ticks the host's clock says have gone by since the last time anyone
 * asked, consuming them from the running total.
 *
 * Split out because there are now two ways time gets delivered and they must
 * share one accounting. co_cpu_idle drains at the idle boundary as it always
 * has; sysvec_co_timer drains from a real interrupt the host synthesised. If
 * each kept its own idea of when it last looked, whichever ran would hide the
 * elapsed time from the other and the guest's clock would run at some fraction
 * of the host's, differently depending on how busy it was.
 */
static unsigned int co_colinux_take_ticks(void)
{
	unsigned char *pp = (unsigned char *)co_colinux_passage_page;
	const u64 per_tick = 10000000 / HZ;	/* 100 ns units */
	static u64 last_mono;
	u64 now;
	unsigned int n;

	if (!pp || !co_colinux_clockevent.event_handler)
		return 0;

	now = *(volatile u64 *)(pp + CO_PP_HOST_MONO);
	if (!last_mono)
		last_mono = now;
	if (now <= last_mono)
		return 0;

	n = (now - last_mono) / per_tick;
	if (!n)
		return 0;

	/*
	 * Clamped, and the clamp forgets rather than owes -- see the note in
	 * co_colinux_drain_time below, which this used to be part of.
	 */
	if (n > HZ) {
		n = HZ;
		last_mono = now - (u64)n * per_tick;
	}
	last_mono += (u64)n * per_tick;

	return n;
}

/*
 * The cooperative timer interrupt: the guest's only asynchronous entry.
 *
 * Reached because the host decided a tick was due and pointed its resume at
 * asm_sysvec_co_timer over a frame it built from the one the interrupt stub had
 * already saved. By the time this runs, the stock entry path has done
 * everything a real interrupt would: the swapgs decision from the interrupted
 * CS, the switch to the interrupt stack, irq_enter. And on the way out,
 * irqentry_exit does the part that matters most -- it is what actually
 * preempts, either by scheduling a kernel-mode return or by running the
 * exit-to-user work loop.
 *
 * That is the whole reason this is a sysvec and not a function the host calls.
 * Delivering ticks by calling into the guest would advance the clock and wake
 * the sleepers, and still never take the processor away from the task that was
 * running -- which is the thing that had to be fixed.
 */
/*
 * The cooperative block device's completion reaper (drivers/block/cobd.c).
 * Called on the same cooperative points that drain time, so a finished async
 * transfer is completed within one tick. A no-op when async block I/O is off
 * (the ring is empty), so it is always safe to call.
 */
void co_cobd_drain_completions(void);

DEFINE_IDTENTRY_SYSVEC(sysvec_co_timer)
{
	struct pt_regs *old = set_irq_regs(regs);
	unsigned int n = co_colinux_take_ticks();

	while (n--)
		co_colinux_clockevent.event_handler(&co_colinux_clockevent);

	co_cobd_drain_completions();

	set_irq_regs(old);
}

void co_colinux_drain_time(void)
{
	struct pt_regs *old, regs;
	unsigned int n;

	n = co_colinux_take_ticks();
	if (!n) {
		/*
		 * Reap block completions even when no tick is due: a task
		 * sleeping on a read drove the guest to idle, and this is the
		 * boundary where its I/O comes back and wakes it.
		 */
		co_cobd_drain_completions();
		return;
	}

	/*
	 * A register frame for the tick to look at.
	 *
	 * tick_periodic() asks user_mode(get_irq_regs()) whether the tick
	 * interrupted userspace, and profile_tick() wants the interrupted RIP.
	 * On real hardware the interrupt entry path fills that pointer in;
	 * these ticks have no entry path, so it stays NULL and user_mode()
	 * dereferences it -- a fault at NULL + 0x88, which is where cs sits in
	 * struct pt_regs, and exactly what the first run of this code did.
	 *
	 * The honest answer is kernel mode: ticks are drained at the idle
	 * boundary, so the guest was in co_cpu_idle when time passed, never in
	 * userspace. Time is therefore accounted to the idle task, which is
	 * what actually consumed it.
	 */
	memset(&regs, 0, sizeof(regs));
	regs.cs	   = __KERNEL_CS;
	regs.ss	   = __KERNEL_DS;
	regs.ip	   = _THIS_IP_;
	regs.sp	   = (unsigned long)&regs;

	irq_enter();
	old = set_irq_regs(&regs);
	while (n--)
		co_colinux_clockevent.event_handler(&co_colinux_clockevent);
	co_cobd_drain_completions();
	set_irq_regs(old);
	irq_exit();
}

/*
 * The host's wall clock, read through the passage page. This is what
 * timekeeping_init and the RTC path ask the platform for; without it the guest
 * boots into 1970 and says 'Unable to read current time from RTC'.
 */
void co_colinux_get_wallclock(struct timespec64 *now)
{
	unsigned char *pp = (unsigned char *)co_colinux_passage_page;

	now->tv_sec  = pp ? *(volatile u64 *)(pp + CO_PP_HOST_WALL) : 0;
	now->tv_nsec = 0;
}

/*
 * The cooperative idle routine: hand the CPU back to the host and resume when
 * it re-enters us. This is coLinux's co_idle_processor, one architecture over.
 * The host sleeps a tick before re-entering, so the drain on the way back is
 * what advances jiffies while the guest is idle -- which is what lets a
 * sleeping boot task (schedule_timeout, msleep) ever wake again.
 */
static void co_cpu_idle(void)
{
	co_colinux_switch(CO_OPERATION_IDLE);
	/*
	 * Interrupts off before the ticks are delivered: irq_enter/irq_exit
	 * emulate hardirq entry, and irq_exit expects to be entered that way.
	 */
	raw_local_irq_disable();
	co_colinux_drain_time();
	/*
	 * And on again, because that is this function's contract: the generic
	 * idle loop calls arch_cpu_idle() with interrupts disabled and every
	 * implementation returns with them enabled -- default_idle ends in
	 * "sti; hlt", and do_idle warns if it comes back otherwise.
	 *
	 * On real hardware the flag is a per-CPU register that each task
	 * carries in its own saved EFLAGS, so leaving it clear would end with
	 * this task. The guest's flag is one global variable, so leaving it
	 * clear ends with the *machine*: the idle task disables, returns, and
	 * every task scheduled afterwards sees interrupts disabled forever.
	 * PID 1 then walked into BUG_ON(irqs_disabled()) in the buffer cache
	 * while looking up /sbin/init, which is a long way from the leak.
	 */
	raw_local_irq_enable();
}

static bool x86_idle_set(void)
{
	return !!static_call_query(x86_idle);
}

#ifndef CONFIG_SMP
static inline void __noreturn play_dead(void)
{
	BUG();
}
#endif

void arch_cpu_idle_enter(void)
{
	tsc_verify_tsc_adjust(false);
	local_touch_nmi();
}

void __noreturn arch_cpu_idle_dead(void)
{
	play_dead();
}

/*
 * Called from the generic idle code.
 */
void __cpuidle arch_cpu_idle(void)
{
	static_call(x86_idle)();
}
EXPORT_SYMBOL_GPL(arch_cpu_idle);

#ifdef CONFIG_XEN
bool xen_set_default_idle(void)
{
	bool ret = x86_idle_set();

	static_call_update(x86_idle, default_idle);

	return ret;
}
#endif

struct cpumask cpus_stop_mask;

void __noreturn stop_this_cpu(void *dummy)
{
	struct cpuinfo_x86 *c = this_cpu_ptr(&cpu_info);
	unsigned int cpu = smp_processor_id();

	local_irq_disable();

	/*
	 * Remove this CPU from the online mask and disable it
	 * unconditionally. This might be redundant in case that the reboot
	 * vector was handled late and stop_other_cpus() sent an NMI.
	 *
	 * According to SDM and APM NMIs can be accepted even after soft
	 * disabling the local APIC.
	 */
	set_cpu_online(cpu, false);
	disable_local_APIC();
	mcheck_cpu_clear(c);

	if (this_cpu_read(cache_state_incoherent))
		wbinvd();

	/*
	 * This brings a cache line back and dirties it, but
	 * native_stop_other_cpus() will overwrite cpus_stop_mask after it
	 * observed that all CPUs reported stop. This write will invalidate
	 * the related cache line on this CPU.
	 */
	cpumask_clear_cpu(cpu, &cpus_stop_mask);

#ifdef CONFIG_SMP
	if (smp_ops.stop_this_cpu) {
		smp_ops.stop_this_cpu();
		BUG();
	}
#endif

	for (;;) {
		/*
		 * Use native_halt() so that memory contents don't change
		 * (stack usage and variables) after possibly issuing the
		 * wbinvd() above.
		 */
		native_halt();
	}
}

/*
 * Prefer MWAIT over HALT if MWAIT is supported, MWAIT_CPUID leaf
 * exists and whenever MONITOR/MWAIT extensions are present there is at
 * least one C1 substate.
 *
 * Do not prefer MWAIT if MONITOR instruction has a bug or idle=nomwait
 * is passed to kernel commandline parameter.
 */
static __init bool prefer_mwait_c1_over_halt(void)
{
	const struct cpuinfo_x86 *c = &boot_cpu_data;
	u32 eax, ebx, ecx, edx;

	/* If override is enforced on the command line, fall back to HALT. */
	if (boot_option_idle_override != IDLE_NO_OVERRIDE)
		return false;

	/* MWAIT is not supported on this platform. Fallback to HALT */
	if (!cpu_has(c, X86_FEATURE_MWAIT))
		return false;

	/* Monitor has a bug or APIC stops in C1E. Fallback to HALT */
	if (boot_cpu_has_bug(X86_BUG_MONITOR) || boot_cpu_has_bug(X86_BUG_AMD_APIC_C1E))
		return false;

	cpuid(CPUID_LEAF_MWAIT, &eax, &ebx, &ecx, &edx);

	/*
	 * If MWAIT extensions are not available, it is safe to use MWAIT
	 * with EAX=0, ECX=0.
	 */
	if (!(ecx & CPUID5_ECX_EXTENSIONS_SUPPORTED))
		return true;

	/*
	 * If MWAIT extensions are available, there should be at least one
	 * MWAIT C1 substate present.
	 */
	return !!(edx & MWAIT_C1_SUBSTATE_MASK);
}

/*
 * MONITOR/MWAIT with no hints, used for default C1 state. This invokes MWAIT
 * with interrupts enabled and no flags, which is backwards compatible with the
 * original MWAIT implementation.
 */
static __cpuidle void mwait_idle(void)
{
	if (need_resched())
		return;

	x86_idle_clear_cpu_buffers();

	if (!current_set_polling_and_test()) {
		const void *addr = &current_thread_info()->flags;

		alternative_input("", "clflush (%[addr])", X86_BUG_CLFLUSH_MONITOR, [addr] "a" (addr));
		__monitor(addr, 0, 0);
		if (need_resched())
			goto out;

		__sti_mwait(0, 0);
		raw_local_irq_disable();
	}

out:
	__current_clr_polling();
}

void __init select_idle_routine(void)
{
	/*
	 * A cooperative guest yields at idle rather than halting. Everything
	 * below chooses between mwait and hlt on real hardware; neither is
	 * right here -- both take the core, which is exactly what a
	 * cooperative guest must not do.
	 */
	if (co_colinux_guest) {
		pr_info("using cooperative idle (coLinux)\n");
		static_call_update(x86_idle, co_cpu_idle);
		return;
	}

	if (boot_option_idle_override == IDLE_POLL) {
		if (IS_ENABLED(CONFIG_SMP) && __max_threads_per_core > 1)
			pr_warn_once("WARNING: polling idle and HT enabled, performance may degrade\n");
		return;
	}

	/* Required to guard against xen_set_default_idle() */
	if (x86_idle_set())
		return;

	if (prefer_mwait_c1_over_halt()) {
		pr_info("using mwait in idle threads\n");
		static_call_update(x86_idle, mwait_idle);
	} else if (cpu_feature_enabled(X86_FEATURE_TDX_GUEST)) {
		pr_info("using TDX aware idle routine\n");
		static_call_update(x86_idle, tdx_halt);
	} else {
		static_call_update(x86_idle, default_idle);
	}
}

void amd_e400_c1e_apic_setup(void)
{
	if (boot_cpu_has_bug(X86_BUG_AMD_APIC_C1E)) {
		pr_info("Switch to broadcast mode on CPU%d\n", smp_processor_id());
		local_irq_disable();
		tick_broadcast_force();
		local_irq_enable();
	}
}

void __init arch_post_acpi_subsys_init(void)
{
	u32 lo, hi;

	if (!boot_cpu_has_bug(X86_BUG_AMD_E400))
		return;

	/*
	 * AMD E400 detection needs to happen after ACPI has been enabled. If
	 * the machine is affected K8_INTP_C1E_ACTIVE_MASK bits are set in
	 * MSR_K8_INT_PENDING_MSG.
	 */
	rdmsr(MSR_K8_INT_PENDING_MSG, lo, hi);
	if (!(lo & K8_INTP_C1E_ACTIVE_MASK))
		return;

	boot_cpu_set_bug(X86_BUG_AMD_APIC_C1E);

	if (!boot_cpu_has(X86_FEATURE_NONSTOP_TSC))
		mark_tsc_unstable("TSC halt in AMD C1E");

	if (IS_ENABLED(CONFIG_GENERIC_CLOCKEVENTS_BROADCAST_IDLE))
		static_branch_enable(&arch_needs_tick_broadcast);
	pr_info("System has AMD C1E erratum E400. Workaround enabled.\n");
}

static int __init idle_setup(char *str)
{
	if (!str)
		return -EINVAL;

	if (!strcmp(str, "poll")) {
		pr_info("using polling idle threads\n");
		boot_option_idle_override = IDLE_POLL;
		cpu_idle_poll_ctrl(true);
	} else if (!strcmp(str, "halt")) {
		/* 'idle=halt' HALT for idle. C-states are disabled. */
		boot_option_idle_override = IDLE_HALT;
	} else if (!strcmp(str, "nomwait")) {
		/* 'idle=nomwait' disables MWAIT for idle */
		boot_option_idle_override = IDLE_NOMWAIT;
	} else {
		return -EINVAL;
	}

	return 0;
}
early_param("idle", idle_setup);

unsigned long arch_align_stack(unsigned long sp)
{
	if (!(current->personality & ADDR_NO_RANDOMIZE) && randomize_va_space)
		sp -= get_random_u32_below(8192);
	return sp & ~0xf;
}

unsigned long arch_randomize_brk(struct mm_struct *mm)
{
	if (mmap_is_ia32())
		return randomize_page(mm->brk, SZ_32M);

	return randomize_page(mm->brk, SZ_1G);
}

/*
 * Called from fs/proc with a reference on @p to find the function
 * which called into schedule(). This needs to be done carefully
 * because the task might wake up and we might look at a stack
 * changing under us.
 */
unsigned long __get_wchan(struct task_struct *p)
{
	struct unwind_state state;
	unsigned long addr = 0;

	if (!try_get_task_stack(p))
		return 0;

	for (unwind_start(&state, p, NULL, NULL); !unwind_done(&state);
	     unwind_next_frame(&state)) {
		addr = unwind_get_return_address(&state);
		if (!addr)
			break;
		if (in_sched_functions(addr))
			continue;
		break;
	}

	put_task_stack(p);

	return addr;
}

SYSCALL_DEFINE2(arch_prctl, int, option, unsigned long, arg2)
{
	switch (option) {
	case ARCH_GET_CPUID:
		return get_cpuid_mode();
	case ARCH_SET_CPUID:
		return set_cpuid_mode(arg2);
	case ARCH_GET_XCOMP_SUPP:
	case ARCH_GET_XCOMP_PERM:
	case ARCH_REQ_XCOMP_PERM:
	case ARCH_GET_XCOMP_GUEST_PERM:
	case ARCH_REQ_XCOMP_GUEST_PERM:
		return fpu_xstate_prctl(option, arg2);
	}

	if (!in_ia32_syscall())
		return do_arch_prctl_64(current, option, arg2);

	return -EINVAL;
}

SYSCALL_DEFINE0(ni_syscall)
{
	return -ENOSYS;
}
