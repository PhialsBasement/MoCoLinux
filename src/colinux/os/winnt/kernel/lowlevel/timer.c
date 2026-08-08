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
 */
static KEVENT co_idle_wake_event;
static int    co_idle_wake_ready;

void co_os_idle_wake_init(void)
{
	KeInitializeEvent(&co_idle_wake_event, SynchronizationEvent, FALSE);
	co_idle_wake_ready = 1;
}

void co_os_idle_wake(void)
{
	if (!co_idle_wake_ready)
		return;
	/*
	 * The increment of 1 boosts the waiting monitor thread so it runs now
	 * rather than at the end of the scheduler's queue -- this call exists
	 * to shave latency, and a wake that waits its turn is half a fix.
	 */
	KeSetEvent(&co_idle_wake_event, 1, FALSE);
}

bool_t co_os_idle_wait(unsigned int msecs)
{
	LARGE_INTEGER DueTime;
	NTSTATUS      status;

	if (!co_idle_wake_ready) {
		co_os_msleep(msecs);
		return PFALSE;
	}

	DueTime.QuadPart = (long long)msecs * 10000 * (-1);
	status = KeWaitForSingleObject(&co_idle_wake_event, Executive,
				       KernelMode, FALSE, &DueTime);
	return status == STATUS_SUCCESS ? PTRUE : PFALSE;
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
