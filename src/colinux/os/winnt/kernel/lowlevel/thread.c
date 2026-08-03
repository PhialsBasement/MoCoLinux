/*
 * This source code is a part of coLinux source package.
 *
 * The code is licensed under the GPL. See the COPYING file at
 * the root directory.
 */

/*
 * Kernel system threads, for the cooperative block device's async workers.
 *
 * The transfer runs in the driver, so a worker must be a real kernel thread,
 * not the user-mode CreateThread in os/winnt/user/misc.c.
 *
 * The join is done through an event the worker sets on its way out, NOT by
 * referencing the thread object: the first cut called ObReferenceObjectByHandle
 * on the thread handle and Driver Verifier bugchecked it 0xC4 (the ioctl runs
 * in the daemon's process context, so the handle PsCreateSystemThread returns
 * is a process handle and referencing it that way is a violation). Waiting on a
 * KEVENT the worker signals is context-independent and needs no object
 * reference at all.
 */

#include "../ddk.h"

#include <colinux/os/alloc.h>
#include <colinux/os/kernel/misc.h>

struct co_os_kthread {
	co_os_kthread_func_t func;
	void*		     arg;
	KEVENT		     done;	/* set by the worker as it exits */
};

static void NTAPI co_os_kthread_trampoline(PVOID p)
{
	struct co_os_kthread* t = (struct co_os_kthread*)p;

	t->func(t->arg);

	/*
	 * Announce the exit and touch nothing in t afterwards -- the joiner
	 * frees t the moment this event is seen, so KeSetEvent must be the last
	 * reference to t. PsTerminateSystemThread below uses only the thread's
	 * own state.
	 */
	KeSetEvent(&t->done, 0, FALSE);
	PsTerminateSystemThread(STATUS_SUCCESS);
}

void* co_os_kthread_start(co_os_kthread_func_t func, void* arg)
{
	struct co_os_kthread* t;
	HANDLE	 handle;
	NTSTATUS status;

	t = co_os_malloc(sizeof(*t));
	if (t == NULL)
		return NULL;
	t->func = func;
	t->arg  = arg;
	/* NotificationEvent (manual reset): once the worker exits it stays
	 * signalled, so a join that arrives afterwards still returns. */
	KeInitializeEvent(&t->done, NotificationEvent, FALSE);

	status = PsCreateSystemThread(&handle, THREAD_ALL_ACCESS, NULL, NULL,
				      NULL, co_os_kthread_trampoline, t);
	if (!NT_SUCCESS(status)) {
		co_os_free(t);
		return NULL;
	}

	/*
	 * The handle is not needed -- the thread runs to completion on its own
	 * and the join waits on the event, not the handle. Closing it here does
	 * not disturb the running thread.
	 */
	ZwClose(handle);

	return t;
}

void co_os_kthread_join(void* thread)
{
	struct co_os_kthread* t = (struct co_os_kthread*)thread;

	if (t == NULL)
		return;

	KeWaitForSingleObject(&t->done, Executive, KernelMode, FALSE, NULL);
	co_os_free(t);
}
