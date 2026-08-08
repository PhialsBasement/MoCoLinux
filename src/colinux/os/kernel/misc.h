/*
 * This source code is a part of coLinux source package.
 *
 * Dan Aloni <da-x@colinux.org>, 2003 (c)
 *
 * The code is licensed under the GPL. See the COPYING file at
 * the root directory.
 *
 */

#ifndef __CO_OS_KERNEL_MISC_H__
#define __CO_OS_KERNEL_MISC_H__

#include <colinux/common/common.h>

/*
 * A host physical address. Wider than a host virtual address can be: PAE gives
 * i386 36 bits, and on x86-64 a Win64 unsigned long is only 32 bits, so
 * returning unsigned long here silently truncated every physical address above
 * 4 GB -- including the passage page's own, which the world switch loads into
 * CR3.
 */
typedef unsigned long long co_pa_t;

extern co_pa_t co_os_virt_to_phys(void *addr);
extern co_rc_t co_os_physical_memory_pages(unsigned long long *pages);
extern co_id_t co_os_current_id(void);

/*
 * Keep this thread on the processor it is running on right now, until
 * co_os_unpin_cpu().
 *
 * The world switch captures the host's CPU state -- GDTR, IDTR, TR, the KPCR in
 * GS_BASE -- and restores it after the guest has run. Every one of those is per
 * processor. If the scheduler moves the thread in between, the switch restores
 * one CPU's descriptor tables and per-CPU pointer onto a different CPU, and the
 * host is then running on tables that describe someone else. It does not fault
 * there; it dies later, somewhere unrelated, differently each time.
 *
 * This was measured, not guessed: the host-state check caught GDTR coming back
 * as 0xfffffadf92cb3b40 where it left as 0xfffffadf92ca3b40, exactly 0x10000
 * apart -- the next processor's.
 *
 * The user-mode daemon has pinned its process since 2004, for the same reason,
 * but only on the path that boots a full instance; nothing pins the ioctl
 * paths, and advisory process affinity is the wrong place for a guarantee the
 * driver depends on anyway.
 */
extern void co_os_pin_cpu(void);
extern void co_os_unpin_cpu(void);

/*
 * Pin this thread to a processor chosen by the caller, not the one it happens
 * to be on. The SMP work needs N monitor threads on N *distinct* processors,
 * and "wherever the scheduler left me" cannot promise distinct. Returns false
 * if the processor does not exist or is not active; unpin with
 * co_os_unpin_cpu() as usual.
 */
extern bool_t co_os_pin_cpu_to(unsigned long cpu);

/* How many processors the host has active. */
extern unsigned long co_os_cpu_count(void);

/*
 * Translate an ordinal in the active-processor set into the processor number
 * used by the affinity API.  Active processor numbers are not guaranteed to
 * be contiguous (for example, a host affinity mask may expose 0, 2, 4, 6), so
 * a population count cannot also be used as the largest processor number.
 * Returns (unsigned long)-1 when ordinal is outside the active set.
 */
extern unsigned long co_os_cpu_nth(unsigned long ordinal);

/*
 * A kernel system thread, for the cooperative block device's async workers.
 *
 * The block transfer runs in the driver (ZwReadFile against a kernel handle),
 * so its worker cannot be the user-mode co_os_thread_* in os/user/misc.h -- it
 * has to be a real kernel thread (PsCreateSystemThread) at PASSIVE_LEVEL.
 *
 * co_os_kthread_start returns an opaque object reference or NULL.
 * co_os_kthread_join waits for func to return and drops the reference; the
 * join holds a reference to the KTHREAD object (not the handle), so it is
 * race-free against the thread exiting first. func must return normally; the
 * trampoline calls PsTerminateSystemThread for it.
 */
typedef void (*co_os_kthread_func_t)(void* arg);
extern void* co_os_kthread_start(co_os_kthread_func_t func, void* arg);
extern void  co_os_kthread_join(void* thread);

/*
 * Which processor this thread is on right now.
 *
 * Exists so the host-state check can say whether a value came back wrong
 * because the switch restored the wrong thing, or because the thread is simply
 * somewhere else and the value is right for where it now is. Those two have
 * identical symptoms and opposite fixes.
 */
extern unsigned long co_os_current_cpu(void);

#endif
