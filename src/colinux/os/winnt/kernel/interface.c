/*
 * This source code is a part of coLinux source package.
 *
 * Dan Aloni <da-x@colinux.org>, 2003 (c)
 *
 * The code is licensed under the GPL. See the COPYING file at
 * the root directory.
 *
 */

#include "ddk.h"
#include "driver.h"

#include <colinux/common/libc.h>
#include <colinux/os/alloc.h>
#include <colinux/kernel/manager.h>
#include <colinux/os/current/monitor.h>
#include <colinux/os/current/ioctl.h>

#include "manager.h"

static NTAPI void manager_irp_cancel(
	PDEVICE_OBJECT DeviceObject,
	PIRP Irp)
{
	co_manager_t *manager;
	co_manager_open_desc_t opened = NULL;
	bool_t myIRP = PFALSE;

	fixme_IoSetCancelRoutine(Irp, NULL);

	manager = (co_manager_t *)DeviceObject->DeviceExtension;
	if (manager) {
		opened = (typeof(opened))(Irp->Tail.Overlay.DriverContext[0]);
		if (opened) {
			/*
			 * Clear the slot only if it still holds *this* IRP.
			 * Nulling it unconditionally, which is what this did,
			 * drops a different pending IRP on the floor: nothing
			 * can find it afterwards, so the read never completes
			 * and the handle close that would have cancelled it
			 * sees an empty slot. This runs with the cancel spin
			 * lock held, so the compare and the clear are atomic
			 * against the consumers that claim it.
			 */
			myIRP = opened->os->irp == Irp;
			if (myIRP)
				opened->os->irp = NULL;
		}
	}

	Irp->IoStatus.Status = STATUS_CANCELLED;
	Irp->IoStatus.Information = 0;

	IoReleaseCancelSpinLock(Irp->CancelIrql);

	IoCompleteRequest(Irp, IO_NO_INCREMENT);

	if (myIRP) {
		co_manager_close(manager, opened);
	}
}

static NTSTATUS manager_read(co_manager_t *manager, co_manager_open_desc_t opened,
			     PIRP Irp)
{
	NTSTATUS ntStatus = STATUS_UNSUCCESSFUL;
	co_rc_t rc;
	char *buffer;
	co_queue_t *queue;

	if (!opened->active) {
		ntStatus = STATUS_PIPE_BROKEN;
		Irp->IoStatus.Status = ntStatus;
		Irp->IoStatus.Information = 0;
		IoCompleteRequest(Irp, IO_NO_INCREMENT);
		return ntStatus;
	}

	buffer = Irp->AssociatedIrp.SystemBuffer;

	co_os_mutex_acquire(opened->lock);

	queue = &opened->out_queue;
	if (co_queue_size(queue) != 0) {
		unsigned char *io_buffer_start = Irp->AssociatedIrp.SystemBuffer;
		unsigned char *io_buffer = io_buffer_start;
		unsigned char *io_buffer_end = io_buffer + Irp->IoStatus.Information;

		while (co_queue_size(queue) != 0)
		{
			co_message_queue_item_t *message_item;
			rc = co_queue_peek_tail(queue, (void **)&message_item);
			if (!CO_OK(rc))
				return rc;

			co_message_t *message = message_item->message;
			unsigned long size = message->size + sizeof(*message);
			if (io_buffer + size > io_buffer_end) {
				break;
			}

			rc = co_queue_pop_tail(queue, (void **)&message_item);
			if (!CO_OK(rc))
				break;

			co_queue_free(queue, message_item);
			co_memcpy(io_buffer, message, size);
			io_buffer += size;
			co_os_free(message);
		}

		Irp->IoStatus.Information = io_buffer - io_buffer_start;
		if (Irp->IoStatus.Information == 0)
			Irp->IoStatus.Status = STATUS_BUFFER_OVERFLOW;
		else
			Irp->IoStatus.Status = ntStatus = STATUS_SUCCESS;

		IoCompleteRequest(Irp, IO_NO_INCREMENT);
	} else {
		if (opened->os->irp) {
			ntStatus = STATUS_INVALID_PARAMETER; /* TODO */
			Irp->IoStatus.Status = ntStatus;
			Irp->IoStatus.Information = 0;
			IoCompleteRequest(Irp, IO_NO_INCREMENT);

		} else {
			KIRQL irql;
			IoAcquireCancelSpinLock(&irql);

			opened->ref_count++;
			ntStatus = STATUS_PENDING;
			Irp->IoStatus.Status = ntStatus;
			Irp->Tail.Overlay.DriverContext[0] = opened;
			fixme_IoSetCancelRoutine(Irp, manager_irp_cancel);
			opened->os->irp = Irp;
			IoMarkIrpPending(Irp);

			IoReleaseCancelSpinLock(irql);
		}
	}

	co_os_mutex_release(opened->lock);

	return ntStatus;
}

static NTSTATUS manager_write(co_manager_t *manager, co_manager_open_desc_t opened, PIRP Irp)
{
	NTSTATUS ntStatus = STATUS_UNSUCCESSFUL;

	if (!opened->active) {
		ntStatus = STATUS_PIPE_BROKEN;
		Irp->IoStatus.Status = ntStatus;
		Irp->IoStatus.Information = 0;
		IoCompleteRequest(Irp, IO_NO_INCREMENT);
		return ntStatus;
	}

	if (opened->monitor) {
		char *buffer;
		unsigned long size;
		co_message_t *message;
		unsigned long message_size;
		long size_left;
		long position;

		buffer = Irp->AssociatedIrp.SystemBuffer;
		size = Irp->IoStatus.Information;
		size_left = size;
		position = 0;

		while (size_left > 0) {
			message = (typeof(message))(&buffer[position]);
			message_size = message->size + sizeof(*message);
			size_left -= message_size;
			if (size_left >= 0) {
				co_monitor_message_from_user(opened->monitor, message);
			}
			position += message_size;
		}


		ntStatus = STATUS_SUCCESS;
	} else {
		Irp->IoStatus.Information = 0;
	}

	Irp->IoStatus.Status = ntStatus;
	IoCompleteRequest(Irp, IO_NO_INCREMENT);

	return ntStatus;
}

static NTSTATUS manager_dispatch(PDEVICE_OBJECT DeviceObject, PIRP Irp)
{
	PIO_STACK_LOCATION  irpStack;
	PVOID               ioBuffer;
	ULONG               inputBufferLength;
	ULONG               outputBufferLength;
	ULONG               ioControlCode;
	NTSTATUS            ntStatus = STATUS_SUCCESS;
	co_manager_t        *manager;
	co_rc_t             rc;
	co_manager_ioctl_t  ioctl;
	co_manager_open_desc_t opened;

	Irp->IoStatus.Status = ntStatus;
	Irp->IoStatus.Information = 0;

	irpStack = IoGetCurrentIrpStackLocation(Irp);

	manager = (co_manager_t *)DeviceObject->DeviceExtension;
	if (!manager) {
		ntStatus = STATUS_NO_SUCH_DEVICE;
		goto complete;
	}

	opened = (typeof(opened))(irpStack->FileObject->FsContext);
	if (!opened) {
		switch (irpStack->MajorFunction) {
		case IRP_MJ_CREATE: {
			rc = co_manager_open(manager, &opened);
			if (!CO_OK(rc)) {
				ntStatus = STATUS_INVALID_PARAMETER; /* TODO */
				goto complete;
			}

			irpStack->FileObject->FsContext = opened;
			break;
		}
		case IRP_MJ_CLOSE:
		case IRP_MJ_CLEANUP:
			break;

		default:
			ntStatus = STATUS_NO_SUCH_DEVICE;
			goto complete;
		}
	}

	ioBuffer           = Irp->AssociatedIrp.SystemBuffer;
	inputBufferLength  = irpStack->Parameters.DeviceIoControl.InputBufferLength;
	outputBufferLength = irpStack->Parameters.DeviceIoControl.OutputBufferLength;

	switch (irpStack->MajorFunction) {
	case IRP_MJ_CLOSE:
	case IRP_MJ_CLEANUP:
		if (opened) {
			irpStack->FileObject->FsContext = NULL;
			co_manager_open_desc_deactive_and_close(manager, opened);
		}
		break;

	case IRP_MJ_READ: {
		Irp->IoStatus.Information = irpStack->Parameters.Read.Length;

		if (Irp->MdlAddress == NULL) {
			ntStatus = STATUS_INVALID_PARAMETER;
			Irp->IoStatus.Information = 0;
			goto complete;
		}

		Irp->AssociatedIrp.SystemBuffer = MmGetSystemAddressForMdlSafe(Irp->MdlAddress, NormalPagePriority);
		if (Irp->AssociatedIrp.SystemBuffer == NULL) {
			ntStatus = STATUS_INSUFFICIENT_RESOURCES;
			Irp->IoStatus.Information = 0;
			goto complete;
		}

		ntStatus = manager_read(manager, opened, Irp);
		return ntStatus;
	}

	case IRP_MJ_WRITE: {
		Irp->IoStatus.Information = irpStack->Parameters.Write.Length;

		if (Irp->MdlAddress == NULL) {
			ntStatus = STATUS_INVALID_PARAMETER;
			Irp->IoStatus.Information = 0;
			goto complete;
		}

		Irp->AssociatedIrp.SystemBuffer = MmGetSystemAddressForMdlSafe(Irp->MdlAddress, NormalPagePriority);
		if (Irp->AssociatedIrp.SystemBuffer == NULL) {
			ntStatus = STATUS_INSUFFICIENT_RESOURCES;
			Irp->IoStatus.Information = 0;
			goto complete;
		}

		ntStatus = manager_write(manager, opened, Irp);
		return ntStatus;
	}

	case IRP_MJ_DEVICE_CONTROL: {
		ioControlCode = irpStack->Parameters.DeviceIoControl.IoControlCode;

		if (CO_GET_IOCTL_TYPE(ioControlCode) != CO_DRIVER_TYPE) {
			ntStatus = STATUS_INVALID_PARAMETER;
			break;
		}

		if (CO_GET_IOCTL_MTYPE(ioControlCode) != METHOD_BUFFERED) {
			ntStatus = STATUS_INVALID_PARAMETER;
			break;
		}

		if (CO_GET_IOCTL_ACCESS(ioControlCode) != FILE_ANY_ACCESS) {
			ntStatus = STATUS_INVALID_PARAMETER;
			break;
		}

		ioctl = (co_manager_ioctl_t)(CO_GET_IOCTL_METHOD(ioControlCode));

		/*
		 * IoStatus.Information is a ULONG_PTR, so on Win64 it is wider than the
		 * unsigned long co_manager_ioctl() reports through. Go via a local of
		 * the type the OS-independent side declares rather than handing it a
		 * pointer to a differently-sized object. The value is a count of bytes
		 * written into the ioctl buffer, so it cannot exceed outputBufferLength.
		 */
		{
			unsigned long return_size = 0;

			co_manager_ioctl(manager, ioctl, ioBuffer,
					 inputBufferLength,
					 outputBufferLength,
					 &return_size,
					 opened);

			Irp->IoStatus.Information = return_size;
		}

		/* Intrinsic Success / Failure indictation is returned per ioctl. */

		ntStatus = STATUS_SUCCESS;
		break;
	}
	}

complete:
	Irp->IoStatus.Status = ntStatus;
	IoCompleteRequest(Irp, IO_NO_INCREMENT);
	return ntStatus;
}

static
NTSTATUS
NTAPI
dispatch_wrapper(IN PDEVICE_OBJECT DeviceObject, IN PIRP Irp)
{
	return manager_dispatch(DeviceObject, Irp);
}

static
VOID
NTAPI
driver_unload(IN PDRIVER_OBJECT DriverObject)
{
	WCHAR               deviceLinkBuffer[]  = L"\\DosDevices\\"CO_DRIVER_NAME;
	UNICODE_STRING      deviceLinkUnicodeString;
	co_manager_t        *manager;

	manager = DriverObject->DeviceObject->DeviceExtension;
	if (manager) {
		co_manager_unload(manager);
	}

	RtlInitUnicodeString(&deviceLinkUnicodeString, deviceLinkBuffer);

	IoDeleteSymbolicLink(&deviceLinkUnicodeString);
	IoDeleteDevice(DriverObject->DeviceObject);
}

PDEVICE_OBJECT coLinux_DeviceObject;

NTSTATUS
NTAPI
DriverEntry(
	IN PDRIVER_OBJECT  DriverObject,
	IN PUNICODE_STRING  RegistryPath
	)
{
	PDEVICE_OBJECT deviceObject = NULL;
	NTSTATUS ntStatus;
	WCHAR deviceNameBuffer[]  = L"\\Device\\" CO_DRIVER_NAME;
	UNICODE_STRING deviceNameUnicodeString;
	WCHAR deviceLinkBuffer[]  = L"\\DosDevices\\" CO_DRIVER_NAME;
	UNICODE_STRING deviceLinkUnicodeString;
	co_manager_t *manager;
	co_rc_t rc;

	RtlInitUnicodeString (&deviceNameUnicodeString, deviceNameBuffer);

	ntStatus = IoCreateDevice (DriverObject,
				   sizeof(co_manager_t),
				   &deviceNameUnicodeString,
				   CO_DRIVER_TYPE,
				   0,
				   FALSE,
				   &deviceObject);

	if (!NT_SUCCESS(ntStatus))
		return ntStatus;

	manager = (co_manager_t *)deviceObject->DeviceExtension;
	manager->state = CO_MANAGER_STATE_NOT_INITIALIZED;

	RtlInitUnicodeString (&deviceLinkUnicodeString, deviceLinkBuffer);
	ntStatus = IoCreateSymbolicLink (&deviceLinkUnicodeString,
					 &deviceNameUnicodeString);

	if (!NT_SUCCESS(ntStatus)) {
		IoDeleteDevice (deviceObject);
		return ntStatus;
	}

	deviceObject->Flags |= DO_DIRECT_IO;

	DriverObject->MajorFunction[IRP_MJ_CREATE]         =
	DriverObject->MajorFunction[IRP_MJ_CLOSE]          =
	DriverObject->MajorFunction[IRP_MJ_CLEANUP]        =
	DriverObject->MajorFunction[IRP_MJ_READ]           =
	DriverObject->MajorFunction[IRP_MJ_WRITE]          =
	DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = dispatch_wrapper;
	DriverObject->DriverUnload                         = driver_unload;

	rc = co_manager_load(manager);

	/* Needed for IoWorkItem */
	coLinux_DeviceObject = deviceObject;

	return STATUS_SUCCESS;
}
