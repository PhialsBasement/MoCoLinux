/*
 * This source code is a part of coLinux source package.
 *
 * Dan Aloni <da-x@colinux.org>, 2003 (c)
 *
 * The code is licensed under the GPL. See the COPYING file at
 * the root directory.
 *
 */

#include "manager.h"

#include <colinux/common/libc.h>
#include <colinux/os/alloc.h>

static void setup_host_memory_range(co_manager_t *manager, co_osdep_manager_t osdep)
{
	osdep->hostmem_max_physical_address =
		(long long) (manager->hostmem_pages - 1) << CO_ARCH_PAGE_SHIFT;

	/* We don't support PGE yet */
	if (osdep->hostmem_max_physical_address >= 0x100000000LL)
		osdep->hostmem_max_physical_address = 0x100000000LL-1;
}

co_rc_t co_os_manager_init(co_manager_t *manager, co_osdep_manager_t *osdep)
{
	co_rc_t rc = CO_RC(OK);
	int i;
	co_osdep_manager_t dep;

	*osdep = dep = co_os_malloc(sizeof(*dep));
	if (dep == NULL)
		return CO_RC(OUT_OF_MEMORY);

	co_global_manager = manager;

	memset(dep, 0, sizeof(*dep));

	co_list_init(&dep->mdl_list);
	co_list_init(&dep->mapped_allocated_list);
	co_list_init(&dep->pages_unused);
	for (i=0; i < PFN_HASH_SIZE; i++)
		co_list_init(&dep->pages_hash[i]);

	MmResetDriverPaging(&co_global_manager);

	rc = co_os_mutex_create(&dep->mutex);

	setup_host_memory_range(manager, dep);

	return rc;
}

void co_os_manager_free(co_osdep_manager_t osdep)
{
	co_debug("before free: %ld mdls, %ld pages", osdep->mdls_allocated, osdep->pages_allocated);

	co_winnt_free_all_pages(osdep);

	co_os_mutex_destroy(osdep->mutex);

	co_debug("after free: %ld mdls, %ld pages", osdep->mdls_allocated, osdep->pages_allocated);

	co_os_free(osdep);
}

co_rc_t co_os_manager_userspace_open(co_manager_open_desc_t opened)
{
	opened->os = co_os_malloc(sizeof(*opened->os));
	if (!opened->os)
		return CO_RC(OUT_OF_MEMORY);

	memset(opened->os, 0, sizeof(*opened->os));
	opened->os->magic = CO_OPEN_OS_MAGIC;

	/*
	 * Both pointers, at every open, so the close-side report below can be
	 * paired with the open that produced it: a close naming a block no
	 * open ever announced is a stale or wild pointer, and a close naming
	 * one that a previous close already released is a double free. Neither
	 * is distinguishable from the other without this line.
	 */
	co_debug("open: desc %p os %p", opened, opened->os);

	return CO_RC(OK);
}

/*
 * Take ownership of the pended IRP, or find that somebody else already has.
 *
 * The pointer must be read and cleared inside one hold of the cancel spin
 * lock. Reading it outside the lock and clearing it inside -- which is what
 * all three consumers below used to do -- lets two paths observe the same
 * non-NULL IRP before either takes the lock. They then serialize, both clear
 * it (the second harmlessly), and both call IoCompleteRequest on it. The
 * second completion walks an IRP the I/O manager has already freed: bugcheck
 * 0x50, a read of unmapped pool, faulting instruction inside ntoskrnl with
 * the caller of this one frame below it.
 *
 * That is not hypothetical. Three dumps in one day carry the identical
 * signature -- 0x50, parameter 2 zero (a read), parameter 3
 * ntoskrnl+0xaf8a3 -- and the stack of the last one resolves to
 * dispatch_wrapper -> co_manager_close -> co_os_manager_userspace_close.
 * Every client that exits with a read pending races this, so a daemon that
 * opens the driver, polls, and quits is enough; a test harness that does it
 * six hundred times makes it certain.
 *
 * manager_irp_cancel is called by the I/O manager with this same lock held
 * and clears the pointer as well, so the lock is the only thing that can
 * decide which path owns the completion. Completing happens after the
 * release, because IoAcquireCancelSpinLock raises to DISPATCH_LEVEL and the
 * work that follows a completion here takes mutexes.
 */
static PIRP co_os_claim_pended_irp(co_manager_open_desc_t opened)
{
	co_manager_open_desc_os_t os = opened->os;
	KIRQL irql;
	PIRP Irp;

	/*
	 * The same three questions the close path asks, for the same reason:
	 * reading ->irp out of a block that is not ours faults with the cancel
	 * spin lock held, which is a worse place to fault than most.
	 */
	if (!os || ((ULONG_PTR)os & 0xf) != 0 || !MmIsAddressValid((PVOID)os) ||
	    os->magic != CO_OPEN_OS_MAGIC) {
		co_debug_error("claim: desc %p os %p is not usable -- "
			       "not touching its IRP", opened, os);
		return NULL;
	}

	IoAcquireCancelSpinLock(&irql);
	Irp = os->irp;
	if (Irp) {
		fixme_IoSetCancelRoutine(Irp, NULL);
		os->irp = NULL;
	}
	IoReleaseCancelSpinLock(irql);

	return Irp;
}

/*
 * Is this a block this driver allocated, and is it still allocated?
 *
 * Asked before the pool allocator is asked, because the pool allocator's way
 * of answering is a bugcheck. Three tests, cheapest first: pool blocks are
 * 16-byte aligned on x86-64, so an unaligned pointer is not one; the page has
 * to be resident, or reading the magic faults instead of reporting; and the
 * magic itself says whether the block is ours and live.
 *
 * A refusal leaks eight bytes of non-paged pool and prints why. That is the
 * right trade against a bugcheck, which loses the machine, the log, and the
 * chance to find out what the pointer actually was.
 */
static bool_t co_os_open_os_usable(co_manager_open_desc_t opened,
				   co_manager_open_desc_os_t os)
{
	if (!os) {
		co_debug_error("close: desc %p has no os block", opened);
		return PFALSE;
	}

	if (((ULONG_PTR)os & 0xf) != 0) {
		co_debug_error("close: desc %p os %p is not 16-byte aligned -- "
			       "not a pool block, refusing to free it",
			       opened, os);
		return PFALSE;
	}

	if (!MmIsAddressValid((PVOID)os)) {
		co_debug_error("close: desc %p os %p is not resident -- "
			       "refusing to free it", opened, os);
		return PFALSE;
	}

	if (os->magic != CO_OPEN_OS_MAGIC) {
		co_debug_error("close: desc %p os %p magic 0x%llx wrong -- "
			       "already freed or never ours, refusing",
			       opened, os, os->magic);
		return PFALSE;
	}

	return PTRUE;
}

void co_os_manager_userspace_close(co_manager_open_desc_t opened)
{
	co_manager_open_desc_os_t os = opened->os;
	PIRP Irp;

	co_debug("close: desc %p os %p", opened, os);

	if (!co_os_open_os_usable(opened, os)) {
		opened->os = NULL;
		return;
	}

	Irp = co_os_claim_pended_irp(opened);

	if (Irp) {
		Irp->IoStatus.Status = STATUS_CANCELLED;
		Irp->IoStatus.Information = 0;
		IoCompleteRequest(Irp, IO_NO_INCREMENT);
	}

	/*
	 * Cleared before the free, so a second arrival at this block is
	 * refused above rather than freeing it twice.
	 */
	os->magic = 0;
	opened->os = NULL;
	co_os_free(os);
}

bool_t co_os_manager_userspace_try_send_direct(
	co_manager_t *manager,
	co_manager_open_desc_t opened,
	co_message_t *message)
{
	PIRP Irp = co_os_claim_pended_irp(opened);
	unsigned char *io_buffer;
	unsigned long size = message->size + sizeof(*message);
	unsigned long buffer_size;

	if (!Irp)
		return PFALSE;

	/*
	 * The IRP is this path's alone from here: it was taken out of
	 * opened->os->irp under the lock, so no other consumer and no cancel
	 * can reach it. Its fields are therefore safe to read, which they were
	 * not before -- buffer_size used to be read before the claim.
	 */
	co_manager_close(manager, opened);

	buffer_size = Irp->IoStatus.Information;
	io_buffer   = Irp->AssociatedIrp.SystemBuffer;

	if (size <= buffer_size) {
		co_memcpy(io_buffer, message, size);
		Irp->IoStatus.Status = STATUS_SUCCESS;
		Irp->IoStatus.Information = size;
		IoCompleteRequest(Irp, IO_NETWORK_INCREMENT);
		return PTRUE;
	}

	Irp->IoStatus.Status = STATUS_CANCELLED;
	Irp->IoStatus.Information = 0;
	IoCompleteRequest(Irp, IO_NO_INCREMENT);

	return PFALSE;
}

co_rc_t co_os_manager_userspace_eof(co_manager_t *manager, co_manager_open_desc_t opened)
{
	PIRP Irp = co_os_claim_pended_irp(opened);

	if (Irp) {
		co_manager_close(manager, opened);

		Irp->IoStatus.Status = STATUS_PIPE_BROKEN;
		Irp->IoStatus.Information = 0;
		IoCompleteRequest(Irp, IO_NO_INCREMENT);
	}

	return CO_RC(OK);
}

co_id_t co_os_current_id(void)
{
	return (co_id_t)(PsGetCurrentProcessId());
}

/*
 * Pin this thread to the processor it is on. See colinux/os/kernel/misc.h for
 * why the world switch cannot survive without it.
 *
 * KeSetSystemAffinityThread takes effect at the next reschedule, so the
 * processor number is read afterwards rather than before: what matters is
 * where the thread ends up, not where it was when it asked. Nesting is not
 * supported and not needed -- the one caller wraps the whole ioctl.
 *
 * Affinity rather than raising IRQL. Running the monitor loop at
 * DISPATCH_LEVEL would also stop the thread moving, but the loop can run for
 * hundreds of thousands of world switches, and starving every DPC on the
 * machine for that long is its own way of killing the host. Device interrupts
 * still need to be taken between steps -- that is now how the host gets them
 * at all.
 */
void co_os_pin_cpu(void)
{
	KAFFINITY here;

	here = (KAFFINITY)1 << KeGetCurrentProcessorNumber();
	KeSetSystemAffinityThread(here);

	/*
	 * If the scheduler had already decided to move us, the mask above was
	 * computed for the wrong processor. Re-read and re-pin; the second one
	 * cannot be wrong, because affinity is now a single bit.
	 */
	here = (KAFFINITY)1 << KeGetCurrentProcessorNumber();
	KeSetSystemAffinityThread(here);
}

void co_os_unpin_cpu(void)
{
	KeRevertToUserAffinityThread();
}

/*
 * Pin to a processor the caller names. Unlike co_os_pin_cpu() there is no
 * read-back dance: the mask is a single bit chosen up front, so the first
 * KeSetSystemAffinityThread is already right -- the thread migrates to that
 * processor at the reschedule the call itself forces.
 *
 * KeQueryActiveProcessors has been exported since NT4, unlike the Vista-era
 * counting APIs, and this driver still loads on XP x64.
 */
bool_t co_os_pin_cpu_to(unsigned long cpu)
{
	KAFFINITY want;

	if (cpu >= sizeof(KAFFINITY) * 8)
		return PFALSE;

	want = (KAFFINITY)1 << cpu;
	if (!(KeQueryActiveProcessors() & want))
		return PFALSE;

	KeSetSystemAffinityThread(want);
	return PTRUE;
}

unsigned long co_os_cpu_count(void)
{
	KAFFINITY active = KeQueryActiveProcessors();
	unsigned long count = 0;

	while (active) {
		count += (unsigned long)(active & 1);
		active >>= 1;
	}

	return count;
}

unsigned long co_os_current_cpu(void)
{
	return (unsigned long)KeGetCurrentProcessorNumber();
}
