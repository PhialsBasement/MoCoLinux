/*
 * This source code is a part of coLinux source package.
 *
 * Dan Aloni <da-x@colinux.org>, 2003 (c)
 *
 * The code is licensed under the GPL. See the COPYING file at
 * the root directory.
 *
 */

#include "../ddk.h"

#include <colinux/os/alloc.h>
#include <colinux/os/timer.h>
#include <colinux/os/kernel/time.h>

struct co_os_timer {
	KDPC dpc;
	KTIMER ktimer;
	co_os_func_t func;
	void *data;
	long msec;
};

VOID
DDKAPI
co_os_timer_routine(
	IN PKDPC Dpc,
	IN PVOID DeferredContext,
	IN PVOID SystemArgument1,
	IN PVOID SystemArgument2
	)
{
	co_os_timer_t timer = (co_os_timer_t)DeferredContext;

	timer->func(timer->data);
}

co_rc_t co_os_timer_create(co_os_func_t func, void *data,
			   long msec, co_os_timer_t *timer_out)
{
	co_os_timer_t timer;

	*timer_out = NULL;

	timer = co_os_malloc(sizeof(*timer));

	if (timer == NULL)
		return CO_RC(OUT_OF_MEMORY);

	timer->func = func;
	timer->data = data;
	timer->msec = msec;

	*timer_out = timer;

	return CO_RC(OK);
}

co_rc_t co_os_timer_activate(co_os_timer_t timer)
{
	LARGE_INTEGER li;

	li.QuadPart = 0;

	KeInitializeDpc(&timer->dpc, &co_os_timer_routine, (PVOID)timer);
	KeInitializeTimerEx(&timer->ktimer, SynchronizationTimer);
	KeSetTimerEx(&timer->ktimer, li, timer->msec, &timer->dpc);

	return CO_RC(OK);
}

void co_os_timer_deactivate(co_os_timer_t timer)
{
	KeCancelTimer(&timer->ktimer);
}

void co_os_timer_destroy(co_os_timer_t timer)
{
	if (timer != NULL)
		co_os_free(timer);
}

void co_os_msleep(unsigned int msecs)
{
	LARGE_INTEGER DueTime;

	DueTime.QuadPart = (long long)msecs * 10000 * (-1);
	KeDelayExecutionThread(KernelMode, FALSE, &DueTime);
}

/*
 * The idle sleep's wake event. Driver-lifetime, initialised at manager load,
 * so there is no per-run state to retire. SynchronizationEvent: setting it
 * releases exactly one wait and clears itself, so a wake that arrives while
 * the monitor is still inside the crossing is not lost -- it is consumed by
 * the next wait, immediately -- and repeated wakes do not accumulate into a
 * burst of re-entries later.
 *
 * One per vCPU: the event releases a single waiter, so vCPUs must not share
 * one. See colinux/os/timer.h.
 */
static KEVENT co_idle_wake_event[CO_MAX_VCPUS];
static KDPC   co_vcpu_kick_dpc[CO_MAX_VCPUS];
static KSPIN_LOCK co_vcpu_kick_lock[CO_MAX_VCPUS];
static int    co_vcpu_kick_bound[CO_MAX_VCPUS];
static KTIMER co_vcpu_preempt_timer[CO_MAX_VCPUS];
static KDPC   co_vcpu_preempt_dpc[CO_MAX_VCPUS];
static int    co_idle_wake_ready;

/*
 * High-resolution deadline wakes, when the OS has them (Win 8.1+). A KTIMER
 * timeout rounds to the system clock interrupt -- one millisecond at best --
 * and the guest's oneshot timers were measured landing 1.1-1.5 ms late
 * through exactly that rounding. ExAllocateTimer with the high-resolution
 * flag is the kernel sibling of the CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
 * trick and wakes within ~50 us. Resolved at runtime; a host without it
 * falls back to the millisecond wait and simply keeps the coarser floor.
 */
typedef PVOID (NTAPI *co_ex_alloc_timer_t)(PVOID callback, PVOID ctx,
					   ULONG attributes);
typedef BOOLEAN (NTAPI *co_ex_set_timer_t)(PVOID timer, LONGLONG duetime,
					   LONGLONG period, PVOID params);
typedef BOOLEAN (NTAPI *co_ex_cancel_timer_t)(PVOID timer, PVOID params);
#define CO_EX_TIMER_HIGH_RESOLUTION 0x4

/*
 * EXT_SET_PARAMETERS, spelled out: NULL parameters to ExSetTimer leaves the
 * system free to coalesce the expiry onto the clock grid, which measured as
 * a 100 us arm waking at 678 us -- a high-resolution timer in name only.
 * Zero tolerance is the whole point of the exercise.
 */
struct co_ext_set_parameters {
	ULONG	 version;	/* EXT_SET_PARAMETERS_VERSION_0 = 0 */
	ULONG	 reserved;
	LONGLONG no_wake_tolerance;	/* 0: fire exactly on time */
};

static co_ex_alloc_timer_t  co_ex_alloc_timer;
static co_ex_set_timer_t    co_ex_set_timer;
static co_ex_cancel_timer_t co_ex_cancel_timer;
static PVOID  co_idle_hires_timer[CO_MAX_VCPUS];
/*
 * The timer's OWN event, never the doorbell's. Sharing the doorbell event
 * looked economical and was a self-amplifying wake storm: a stale timer
 * fire leaves the auto-reset event set, the next wait returns instantly,
 * re-arms, and the census read 47k spurious passes in 30 idle seconds.
 * The doorbell event must never be cleared (a clear can eat a completion
 * wake -- the set-with-no-waiter-arms-the-next-wait contract in timer.h is
 * load-bearing); a private event may be cleared freely because only the
 * timer sets it. Waits take WaitAny over the pair.
 */
static KEVENT co_idle_hires_event[CO_MAX_VCPUS];

static VOID NTAPI co_idle_hires_callback(PVOID timer, PVOID ctx)
{
	unsigned long vcpu = (unsigned long)(ULONG_PTR)ctx;

	(void)timer;
	if (vcpu < CO_MAX_VCPUS)
		KeSetEvent(&co_idle_hires_event[vcpu], 1, FALSE);
}

static void co_os_idle_hires_measure(void);

static VOID DDKAPI co_os_vcpu_kick_dpc_routine(
	IN PKDPC Dpc,
	IN PVOID DeferredContext,
	IN PVOID SystemArgument1,
	IN PVOID SystemArgument2)
{
	(void)Dpc;
	(void)DeferredContext;
	(void)SystemArgument1;
	(void)SystemArgument2;
}

/*
 * Xen's VIRQ_TIMER equivalent.
 *
 * The body is empty for the same reason as the posted-IPI DPC above.  Expiring
 * a high-priority DPC on the processor carrying a running vCPU sends that
 * processor a Windows interrupt.  The interrupt enters the guest's hybrid IDT,
 * crosses to the monitor, and is replayed into Windows.  At that point the
 * monitor owns a complete stopped userspace frame and can deliver the guarded
 * cooperative timer upcall before resuming it.
 *
 * Without this targeted source, periodic injection was merely *checked* after
 * unrelated host interrupts.  A quiet processor can go tens of seconds
 * without one, which made a two-second guest sleep take fifty-two seconds.
 */
static VOID DDKAPI co_os_vcpu_preempt_dpc_routine(
	IN PKDPC Dpc,
	IN PVOID DeferredContext,
	IN PVOID SystemArgument1,
	IN PVOID SystemArgument2)
{
	(void)Dpc;
	(void)DeferredContext;
	(void)SystemArgument1;
	(void)SystemArgument2;
}

void co_os_idle_wake_init(void)
{
	unsigned long i;

	for (i = 0; i < CO_MAX_VCPUS; i++) {
		KeInitializeEvent(&co_idle_wake_event[i], SynchronizationEvent, FALSE);
		KeInitializeEvent(&co_idle_hires_event[i], SynchronizationEvent, FALSE);
		KeInitializeDpc(&co_vcpu_kick_dpc[i],
				&co_os_vcpu_kick_dpc_routine, NULL);
		KeSetImportanceDpc(&co_vcpu_kick_dpc[i], HighImportance);
		KeInitializeSpinLock(&co_vcpu_kick_lock[i]);
		co_vcpu_kick_bound[i] = 0;
		KeInitializeTimerEx(&co_vcpu_preempt_timer[i], NotificationTimer);
		KeInitializeDpc(&co_vcpu_preempt_dpc[i],
				&co_os_vcpu_preempt_dpc_routine, NULL);
		KeSetImportanceDpc(&co_vcpu_preempt_dpc[i], HighImportance);
	}

	{
		UNICODE_STRING name;

		RtlInitUnicodeString(&name, L"ExAllocateTimer");
		co_ex_alloc_timer = (co_ex_alloc_timer_t)
			MmGetSystemRoutineAddress(&name);
		RtlInitUnicodeString(&name, L"ExSetTimer");
		co_ex_set_timer = (co_ex_set_timer_t)
			MmGetSystemRoutineAddress(&name);
		RtlInitUnicodeString(&name, L"ExCancelTimer");
		co_ex_cancel_timer = (co_ex_cancel_timer_t)
			MmGetSystemRoutineAddress(&name);

		if (co_ex_alloc_timer && co_ex_set_timer && co_ex_cancel_timer) {
			for (i = 0; i < CO_MAX_VCPUS; i++)
				co_idle_hires_timer[i] = co_ex_alloc_timer(
					(PVOID)co_idle_hires_callback,
					(PVOID)(ULONG_PTR)i,
					CO_EX_TIMER_HIGH_RESOLUTION);
		}
	}

	co_os_idle_hires_measure();

	co_idle_wake_ready = 1;
}

void co_os_idle_wake_shutdown(void)
{
	unsigned long i;

	if (!co_idle_wake_ready)
		return;

	co_idle_wake_ready = 0;
	for (i = 0; i < CO_MAX_VCPUS; i++) {
		KIRQL old_irql;

		KeAcquireSpinLock(&co_vcpu_kick_lock[i], &old_irql);
		co_vcpu_kick_bound[i] = 0;
		KeCancelTimer(&co_vcpu_preempt_timer[i]);
		KeRemoveQueueDpc(&co_vcpu_kick_dpc[i]);
		KeRemoveQueueDpc(&co_vcpu_preempt_dpc[i]);
		KeReleaseSpinLock(&co_vcpu_kick_lock[i], old_irql);
	}

	/* A DPC already taken off its queue may still be executing our code. */
	KeFlushQueuedDpcs();
}

void co_os_idle_wake(unsigned long vcpu)
{
	if (!co_idle_wake_ready || vcpu >= CO_MAX_VCPUS)
		return;
	/*
	 * The increment of 1 boosts the waiting monitor thread so it runs now
	 * rather than at the end of the scheduler's queue -- this call exists
	 * to shave latency, and a wake that waits its turn is half a fix.
	 */
	KeSetEvent(&co_idle_wake_event[vcpu], 1, FALSE);
}

void co_os_idle_wake_all(void)
{
	unsigned long i;

	for (i = 0; i < CO_MAX_VCPUS; i++)
		co_os_idle_wake(i);
}

/*
 * Ring a running vCPU, not only one parked in co_os_idle_wait().
 *
 * KeSetTargetProcessorDpc selects the Windows processor carrying the target
 * vCPU, and HighImportance makes KeInsertQueueDpc begin processing that
 * processor's queue immediately. If the target is executing guest code, the
 * resulting host IPI first enters the guest's hybrid IDT and crosses to its
 * monitor loop. The loop replays the IPI into Windows, then uses that stopped
 * guest frame to deliver the posted cooperative IPI before resuming it.
 *
 * The DPC body is intentionally empty. Its interrupt is the doorbell; the
 * message itself is already in co_colinux_ipi_pending[].
 */
bool_t co_os_vcpu_kick_bind(unsigned long vcpu, unsigned long host_cpu)
{
	KIRQL old_irql;

	if (!co_idle_wake_ready || vcpu >= CO_MAX_VCPUS ||
	    host_cpu >= sizeof(KAFFINITY) * 8)
		return PFALSE;

	KeAcquireSpinLock(&co_vcpu_kick_lock[vcpu], &old_irql);
	/* A proper release drains this first; remove defensively for a slot
	 * recovered after an interrupted bring-up. */
	KeRemoveQueueDpc(&co_vcpu_kick_dpc[vcpu]);
	KeSetTargetProcessorDpc(&co_vcpu_kick_dpc[vcpu], (CCHAR)host_cpu);
	co_vcpu_kick_bound[vcpu] = 1;
	KeReleaseSpinLock(&co_vcpu_kick_lock[vcpu], old_irql);

	return PTRUE;
}

void co_os_vcpu_kick_unbind(unsigned long vcpu)
{
	KIRQL old_irql;

	if (!co_idle_wake_ready || vcpu >= CO_MAX_VCPUS)
		return;

	/* Closing the gate under the same lock as queueing means a sender that
	 * observed the old vCPU cannot enqueue after the drain. */
	KeAcquireSpinLock(&co_vcpu_kick_lock[vcpu], &old_irql);
	co_vcpu_kick_bound[vcpu] = 0;
	KeRemoveQueueDpc(&co_vcpu_kick_dpc[vcpu]);
	KeReleaseSpinLock(&co_vcpu_kick_lock[vcpu], old_irql);

	/* The routine is empty, but it still lives in this driver image.  Make
	 * slot reuse and eventual unload wait for an already-running instance. */
	KeFlushQueuedDpcs();
}

void co_os_vcpu_kick(unsigned long vcpu)
{
	KIRQL old_irql;

	co_os_idle_wake(vcpu);

	if (!co_idle_wake_ready || vcpu >= CO_MAX_VCPUS)
		return;

	KeAcquireSpinLock(&co_vcpu_kick_lock[vcpu], &old_irql);
	if (co_vcpu_kick_bound[vcpu])
		KeInsertQueueDpc(&co_vcpu_kick_dpc[vcpu], NULL, NULL);
	KeReleaseSpinLock(&co_vcpu_kick_lock[vcpu], old_irql);
}

void co_os_vcpu_preempt_start(unsigned long vcpu, unsigned long host_cpu,
			      unsigned int period_msec)
{
	LARGE_INTEGER due;

	if (!co_idle_wake_ready || vcpu >= CO_MAX_VCPUS ||
	    host_cpu >= sizeof(KAFFINITY) * 8 || period_msec == 0)
		return;

	/* Re-arming a lane first retires any timer left by an earlier run. */
	KeCancelTimer(&co_vcpu_preempt_timer[vcpu]);
	KeRemoveQueueDpc(&co_vcpu_preempt_dpc[vcpu]);
	KeSetTargetProcessorDpc(&co_vcpu_preempt_dpc[vcpu], (CCHAR)host_cpu);

	due.QuadPart = -((long long)period_msec * 10000LL);
	KeSetTimerEx(&co_vcpu_preempt_timer[vcpu], due, (LONG)period_msec,
		     &co_vcpu_preempt_dpc[vcpu]);
}

void co_os_vcpu_preempt_stop(unsigned long vcpu)
{
	if (vcpu >= CO_MAX_VCPUS)
		return;

	KeCancelTimer(&co_vcpu_preempt_timer[vcpu]);
	KeRemoveQueueDpc(&co_vcpu_preempt_dpc[vcpu]);
}

bool_t co_os_idle_wait(unsigned long vcpu, unsigned int msecs)
{
	LARGE_INTEGER DueTime;
	NTSTATUS      status;

	if (!co_idle_wake_ready || vcpu >= CO_MAX_VCPUS) {
		co_os_msleep(msecs ? msecs : 1);
		return PFALSE;
	}

	/*
	 * Zero means no timeout at all. No caller passes it today -- see the
	 * contract in os/timer.h.
	 *
	 * The temptation is obvious: every producer rings the doorbell now, so a
	 * timeout looks like a pure poll, and a poll here is a full world switch
	 * -- CR3, GDT, IDT, TR, fourteen MSRs, FXSAVE -- taken to find an empty
	 * operation slot. But the monitor's timeout is also the guest's clock:
	 * ticks are synthesised from elapsed host time only when a vCPU wakes and
	 * re-enters, so removing it stops time on that processor. This path
	 * exists for the day the guest publishes its next expiry instead of
	 * asking for a periodic tick.
	 */
	if (msecs == 0) {
		status = KeWaitForSingleObject(&co_idle_wake_event[vcpu],
					       Executive, KernelMode, FALSE, NULL);
		return status == STATUS_SUCCESS ? PTRUE : PFALSE;
	}

	DueTime.QuadPart = (long long)msecs * 10000 * (-1);
	status = KeWaitForSingleObject(&co_idle_wake_event[vcpu], Executive,
				       KernelMode, FALSE, &DueTime);
	return status == STATUS_SUCCESS ? PTRUE : PFALSE;
}

/*
 * Wait until an ABSOLUTE host-monotonic deadline (100 ns units), waking
 * within ~50 us of it when the OS has high-resolution timers -- the wake the
 * guest's oneshot clockevents are only as good as. Doorbells cut it short
 * exactly as they cut the millisecond wait short: same event, one waiter.
 * Returns PTRUE when woken by the doorbell rather than the deadline.
 *
 * Falls back to the millisecond wait when high-resolution timers are absent
 * (XP): the guest still gets its event, one clock interrupt late.
 */
/* One bit for the log: did the high-resolution timers actually allocate. */
int co_os_idle_hires_available(void)
{
	return co_idle_hires_timer[0] != NULL;
}

/*
 * The timer measured against itself, at init: arm 100 us, clock the actual
 * wake. Every layer above has now been rewritten twice on the ASSUMPTION
 * that this primitive is precise; this number replaces the assumption. In
 * 100 ns units; 0 = never ran, ~1000 = the promise kept, ~10000 = it is a
 * millisecond timer wearing a flag.
 */
static unsigned long long co_idle_hires_selftest;

unsigned long long co_os_idle_hires_selftest(void)
{
	return co_idle_hires_selftest;
}

static void co_os_idle_hires_measure(void)
{
	struct co_ext_set_parameters p = { 0, 0, 0 };
	unsigned long long best = ~0ULL;
	int i;

	if (!co_idle_hires_timer[0])
		return;

	/*
	 * Min of three: the first arm can pay a one-time clock ramp while
	 * the kernel raises the interrupt rate for the high-res window.
	 */
	for (i = 0; i < 3; i++) {
		unsigned long long t0, dt;
		LARGE_INTEGER backstop;

		KeClearEvent(&co_idle_hires_event[0]);
		t0 = co_os_monotonic_100ns();
		co_ex_set_timer(co_idle_hires_timer[0], -1000LL /* 100 us */,
				0, &p);
		backstop.QuadPart = -100000LL;	/* 10 ms: never fires = bug */
		KeWaitForSingleObject(&co_idle_hires_event[0], Executive,
				      KernelMode, FALSE, &backstop);
		dt = co_os_monotonic_100ns() - t0;
		if (dt < best)
			best = dt;
		co_ex_cancel_timer(co_idle_hires_timer[0], NULL);
	}
	KeClearEvent(&co_idle_hires_event[0]);
	co_idle_hires_selftest = best;
}

bool_t co_os_idle_wait_until(unsigned long vcpu, unsigned long long abs_100ns)
{
	unsigned long long now;
	NTSTATUS status;

	if (!co_idle_wake_ready || vcpu >= CO_MAX_VCPUS) {
		co_os_msleep(1);
		return PFALSE;
	}

	now = co_os_monotonic_100ns();
	if (abs_100ns <= now)
		return PFALSE;

	if (!co_idle_hires_timer[vcpu]) {
		unsigned int ms = (unsigned int)((abs_100ns - now) / 10000ULL);

		return co_os_idle_wait(vcpu, ms ? ms : 1);
	}

	{
		struct co_ext_set_parameters p = { 0, 0, 0 };
		PVOID objs[2];
		LARGE_INTEGER backstop;

		KeClearEvent(&co_idle_hires_event[vcpu]);
		co_ex_set_timer(co_idle_hires_timer[vcpu],
				-(LONGLONG)(abs_100ns - now), 0, &p);

		objs[0] = &co_idle_wake_event[vcpu];
		objs[1] = &co_idle_hires_event[vcpu];
		/* Backstop so a lost timer can never hang a vCPU forever. */
		backstop.QuadPart = -(LONGLONG)(abs_100ns - now) - 100000LL;
		status = KeWaitForMultipleObjects(2, objs, WaitAny, Executive,
						  KernelMode, FALSE, &backstop,
						  NULL);
		co_ex_cancel_timer(co_idle_hires_timer[vcpu], NULL);
		KeClearEvent(&co_idle_hires_event[vcpu]);
	}
	/* STATUS_WAIT_0 = doorbell: tell the caller something happened. */
	return status == STATUS_WAIT_0 ? PTRUE : PFALSE;
}

/*
 * Ask Windows for a finer clock while a guest is running.
 *
 * co_os_msleep above is KeDelayExecutionThread with a relative interval, and
 * Windows rounds that up to the next clock tick. On XP the default tick is
 * 15.6 ms, so every sleep the monitor loop asks for -- one millisecond or ten
 * -- waits for the same tick boundary. That tick is therefore the floor on
 * every round trip the guest makes, because a guest waiting for a reply is
 * idle and the monitor loop sleeps when the guest is idle.
 *
 * It is measurable and it is not subtle: a ping to 10.0.2.2, slirp's gateway,
 * answered by a process on this same machine with no network involved at all,
 * takes 10 ms. So does every TCP round trip, which caps throughput at window
 * over one tick -- a download sat at 1.39 MB/s from a mirror 5 ms away and
 * 1.64 MB/s from one on another continent, which is what a limit inside the
 * host looks like from outside.
 *
 * ExSetTimerResolution is the documented way to ask for better, and it is what
 * multimedia timers use. It is system-wide and reference counted, so it is
 * claimed when a run starts and released when it ends rather than left on: a
 * faster clock costs the whole machine power and interrupts, and a driver that
 * quietly keeps it forever is a bad neighbour on a laptop.
 *
 * Requesting is not getting. The kernel returns the resolution it actually
 * adopted, which is its own tick if it will not go finer, so the caller logs
 * what it got rather than assuming.
 */
#define CO_TIMER_WANT_100NS	10000	/* 1 ms */

unsigned long co_os_timer_resolution_acquire(void)
{
	return ExSetTimerResolution(CO_TIMER_WANT_100NS, TRUE);
}

void co_os_timer_resolution_release(void)
{
	ExSetTimerResolution(0, FALSE);
}
