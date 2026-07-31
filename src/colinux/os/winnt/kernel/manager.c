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

	return CO_RC(OK);
}

void co_os_manager_userspace_close(co_manager_open_desc_t opened)
{
	KIRQL irql;
	PIRP Irp;

	Irp = opened->os->irp;
	if (Irp) {
		IoAcquireCancelSpinLock(&irql);
		fixme_IoSetCancelRoutine(Irp, NULL);
		opened->os->irp = NULL;
		IoReleaseCancelSpinLock(irql);

		Irp->IoStatus.Status = STATUS_CANCELLED;
		Irp->IoStatus.Information = 0;
		IoCompleteRequest(Irp, IO_NO_INCREMENT);
	}

	co_os_free(opened->os);
	opened->os = NULL;
}

bool_t co_os_manager_userspace_try_send_direct(
	co_manager_t *manager,
	co_manager_open_desc_t opened,
	co_message_t *message)
{
	PIRP Irp;

	Irp = opened->os->irp;
	if (Irp) {
		KIRQL irql;
		unsigned char *io_buffer;
		unsigned long size = message->size + sizeof(*message);
		unsigned long buffer_size = Irp->IoStatus.Information;

		IoAcquireCancelSpinLock(&irql);
		fixme_IoSetCancelRoutine(Irp, NULL);
		opened->os->irp = NULL;
		IoReleaseCancelSpinLock(irql);

		co_manager_close(manager, opened);

		io_buffer = Irp->AssociatedIrp.SystemBuffer;

		if (size <= buffer_size) {
			co_memcpy(io_buffer, message, size);
			Irp->IoStatus.Status = STATUS_SUCCESS;
			Irp->IoStatus.Information = size;
			IoCompleteRequest(Irp, IO_NETWORK_INCREMENT);
			return PTRUE;
		} else {
			Irp->IoStatus.Status = STATUS_CANCELLED;
			Irp->IoStatus.Information = 0;
			IoCompleteRequest(Irp, IO_NO_INCREMENT);
		}
	}

	return PFALSE;
}

co_rc_t co_os_manager_userspace_eof(co_manager_t *manager, co_manager_open_desc_t opened)
{
	PIRP Irp;

	Irp = opened->os->irp;
	if (Irp) {
		KIRQL irql;

		IoAcquireCancelSpinLock(&irql);
		fixme_IoSetCancelRoutine(Irp, NULL);
		opened->os->irp = NULL;
		IoReleaseCancelSpinLock(irql);

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

unsigned long co_os_current_cpu(void)
{
	return (unsigned long)KeGetCurrentProcessorNumber();
}
