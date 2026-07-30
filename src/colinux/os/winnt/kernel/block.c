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

#include <ntdddisk.h>

#include <colinux/common/libc.h>
#include <colinux/os/alloc.h>
#include <colinux/kernel/transfer.h>
#include <colinux/kernel/fileblock.h>
#include <colinux/kernel/monitor.h>

#include "fileio.h"

typedef struct {
	struct {
		co_message_t message;
		co_linux_message_t linux_message;
		co_block_intr_t intr;
	} msg; /* Must stay as the first field */
	co_monitor_t *monitor;
	co_pfn_t pfn;
	unsigned char *page;
	unsigned char *start;
	IO_STATUS_BLOCK isb;
	LARGE_INTEGER offset;
	unsigned long size;
} callback_context_t;

typedef struct {
	HANDLE file_handle;
	LARGE_INTEGER offset;
} co_os_transfer_file_block_data_t;

static co_rc_t transfer_file_block(co_monitor_t *cmon,
				  void *host_data, void *linuxvm,
				  unsigned long size,
				  co_monitor_transfer_dir_t dir)
{
	IO_STATUS_BLOCK isb;
	NTSTATUS status;
	co_os_transfer_file_block_data_t *data;
	co_rc_t rc = CO_RC_OK;

	data = (co_os_transfer_file_block_data_t *)host_data;

	if (CO_MONITOR_TRANSFER_FROM_HOST == dir) {
		status = ZwReadFile(data->file_handle,
				    NULL,
				    NULL,
				    NULL,
				    &isb,
				    linuxvm,
				    size,
				    &data->offset,
				    NULL);
	}
	else {
		status = ZwWriteFile(data->file_handle,
				     NULL,
				     NULL,
				     NULL,
				     &isb,
				     linuxvm,
				     size,
				     &data->offset,
				     NULL);
	}

	if (status != STATUS_SUCCESS) {
		co_debug_error("block io failed: %p %lx (reason: %x)",
				linuxvm, size, (int)status);
		rc = co_status_convert(status);
	}

	data->offset.QuadPart += size;

	return rc;
}

co_rc_t co_os_file_block_read_write(co_monitor_t *monitor,
				    HANDLE file_handle,
				    unsigned long long offset,
				    vm_ptr_t address,
				    unsigned long size,
				    bool_t read)
{
	co_rc_t rc;
	co_os_transfer_file_block_data_t data;

	data.offset.QuadPart = offset;
	data.file_handle = file_handle;

	rc = co_monitor_host_linuxvm_transfer(monitor,
					      &data,
					      transfer_file_block,
					      address,
					      size,
					      (read ? CO_MONITOR_TRANSFER_FROM_HOST :
					       CO_MONITOR_TRANSFER_FROM_LINUX));

	return rc;
}

/*
 * CALLBACK is a user-mode windef.h macro and is not defined by the kernel-mode
 * headers. This is a PIO_APC_ROUTINE, so NTAPI is the right spelling of the
 * same __stdcall convention.
 */
static void NTAPI transfer_file_block_callback(callback_context_t *context, PIO_STATUS_BLOCK IoStatusBlock, ULONG Reserved)
{
	co_debug_lvl(filesystem, 10, "cobd%d callback size=%ld info=%ld status=%X",
			context->msg.linux_message.unit, context->size,
			(long)IoStatusBlock->Information, (int)IoStatusBlock->Status);

	if (IoStatusBlock->Status == STATUS_SUCCESS)
		context->msg.intr.uptodate = 1;
	else
		co_debug("cobd%d callback failed size=%ld info=%ld status=%X",
			    context->msg.linux_message.unit, context->size,
			    (unsigned long)IoStatusBlock->Information,
			    (int)IoStatusBlock->Status);

	co_monitor_host_linuxvm_transfer_unmap(context->monitor, context->page, context->pfn);
	co_monitor_message_from_user_free(context->monitor, &context->msg.message);
}

static co_rc_t co_os_file_block_async_read_write(co_monitor_t *monitor,
				    HANDLE file_handle,
				    unsigned long long offset,
				    vm_ptr_t address,
				    unsigned long size,
				    bool_t read,
				    int unit,
				    void *irq_request)
{
	co_rc_t rc;
	NTSTATUS status;

	// Prepare callback
	callback_context_t *context = co_os_malloc(sizeof (callback_context_t));
	if (!context)
		return CO_RC(OUT_OF_MEMORY);
	co_memset(context, 0, sizeof(callback_context_t));

	context->monitor = monitor;
	context->offset.QuadPart = offset;
	context->size = size;
	context->msg.message.from = CO_MODULE_COBD0 + unit;
	context->msg.message.to = CO_MODULE_LINUX;
	context->msg.message.priority = CO_PRIORITY_DISCARDABLE;
	context->msg.message.type = CO_MESSAGE_TYPE_OTHER;
	context->msg.message.size = sizeof (context->msg) - sizeof (context->msg.message);
	context->msg.linux_message.device = CO_DEVICE_BLOCK;
	context->msg.linux_message.unit = unit;
	context->msg.linux_message.size = sizeof (context->msg.intr);
	context->msg.intr.irq_request = irq_request;

	// map linux kernal memory into host memory
	rc = co_monitor_host_linuxvm_transfer_map(
		monitor,
		address,
		size,
		&context->start,
		&context->page,
		&context->pfn);

	if (CO_OK(rc)) {
		if (read) {
			status = ZwReadFile(file_handle,
				    NULL,
				    (PIO_APC_ROUTINE) transfer_file_block_callback,
				    context,
				    &context->isb,
				    context->start,
				    size,
				    &context->offset,
				    NULL);
			co_debug_lvl(filesystem, 10, "cobd%d read status %X", unit, (int)status);
		} else {
			status = ZwWriteFile(file_handle,
				     NULL,
				     (PIO_APC_ROUTINE) transfer_file_block_callback,
				     context,
				     &context->isb,
				     context->start,
				     size,
				     &context->offset,
				     NULL);
			co_debug_lvl(filesystem, 10, "cobd%d write status %X", unit, (int)status);
		}

		if (status == STATUS_PENDING || status == STATUS_SUCCESS)
			return CO_RC(OK);

		rc = co_status_convert(status);
		co_debug("block io failed: %p %lx (reason: %x)", context->start, size, (int)status);
		co_monitor_host_linuxvm_transfer_unmap(monitor, context->page, context->pfn);
	}

	co_os_free(context);
	return rc;
}

static co_rc_t co_os_file_block_async_read(co_monitor_t *linuxvm, co_block_dev_t *dev,
			      co_monitor_file_block_dev_t *fdev, co_block_request_t *request)
{
	request->async = PTRUE;
	return co_os_file_block_async_read_write(linuxvm, (HANDLE)(fdev->sysdep),
						request->offset, request->address,
						request->size, PTRUE, dev->unit,
						request->irq_request);
}

static co_rc_t co_os_file_block_async_write(co_monitor_t *linuxvm, co_block_dev_t *dev,
			       co_monitor_file_block_dev_t *fdev, co_block_request_t *request)
{
	request->async = PTRUE;
	return co_os_file_block_async_read_write(linuxvm, (HANDLE)(fdev->sysdep),
						request->offset, request->address,
						request->size, PFALSE, dev->unit,
						request->irq_request);
}

static co_rc_t co_os_file_block_read(co_monitor_t *linuxvm, co_block_dev_t *dev,
			      co_monitor_file_block_dev_t *fdev, co_block_request_t *request)
{
	return co_os_file_block_read_write(linuxvm, (HANDLE)(fdev->sysdep),
					   request->offset, request->address,
					   request->size, PTRUE);
}

static co_rc_t co_os_file_block_write(co_monitor_t *linuxvm, co_block_dev_t *dev,
			       co_monitor_file_block_dev_t *fdev, co_block_request_t *request)
{
	return co_os_file_block_read_write(linuxvm, (HANDLE)(fdev->sysdep),
					   request->offset, request->address,
					   request->size, PFALSE);
}

static bool_t probe_area(HANDLE handle, LARGE_INTEGER offset, char *test_buffer, unsigned long size)
{
	IO_STATUS_BLOCK isb;
	NTSTATUS status;

	status = ZwReadFile(handle, NULL, NULL,
			    NULL, &isb, test_buffer, size,
			    &offset, NULL);

	if (status != STATUS_SUCCESS)
		return PFALSE;

	return PTRUE;
}

static co_rc_t co_os_file_block_detect_size_binary_search(HANDLE handle, unsigned long long *out_size)
{
	/*
	 * Binary search the size of the device.
	 *
	 * Yep, it's ugly.
	 *
	 * I haven't found a more reliable way. I thought about switching between
	 * IOCTL_DISK_GET_DRIVE_GEOMETRY, IOCTL_DISK_GET_PARTITION_INFORMATION,
	 * and even IOCTL_CDROM_GET_DRIVE_GEOMETRY depending on the device's type,
	 * but I'm not sure if would work in all cases.
	 *
	 * This *would* work in all cases.
	 */

	LARGE_INTEGER scan_bit;
	LARGE_INTEGER build_size;
	unsigned long test_buffer_size_max = 0x4000;
	unsigned long test_buffer_size = 0x100;
	char *test_buffer;

	test_buffer = co_os_malloc(test_buffer_size_max);
	if (!test_buffer)
		return CO_RC(OUT_OF_MEMORY);

	build_size.QuadPart = 0;

	while (test_buffer_size <= test_buffer_size_max) {
		if (probe_area(handle, build_size, test_buffer, test_buffer_size))
			break;

		test_buffer_size <<= 1;
	}

	if (test_buffer_size >= test_buffer_size_max) {
		co_debug_error("size is zero");
		*out_size = 0;
		co_os_free(test_buffer);
		return CO_RC(ERROR);
	}

	scan_bit.QuadPart = 1;

	/*
	 * Find the smallest invalid power of 2.
	 */
	while (scan_bit.QuadPart != 0) {
		if (!probe_area(handle, scan_bit, test_buffer, test_buffer_size))
			break;
		scan_bit.QuadPart <<= 1;
	}

	if (scan_bit.QuadPart == 0)
		return CO_RC(ERROR);

	while (scan_bit.QuadPart) {
		LARGE_INTEGER with_bit;

		scan_bit.QuadPart >>= 1;
		with_bit.QuadPart = build_size.QuadPart | scan_bit.QuadPart;
		if (probe_area(handle, with_bit, test_buffer, test_buffer_size))
			build_size = with_bit;
	}

	build_size.QuadPart += test_buffer_size;

	*out_size = build_size.QuadPart;
	co_debug("detected size: %llu KBs", (build_size.QuadPart >> 10));

	co_os_free(test_buffer);
	return CO_RC(OK);
}

static co_rc_t co_os_file_block_detect_size_harddisk(HANDLE handle, unsigned long long *out_size)
{
	GET_LENGTH_INFORMATION length;
	IO_STATUS_BLOCK block;
	NTSTATUS status;

	length.Length.QuadPart = 0;

	status = ZwDeviceIoControlFile(handle, NULL, NULL, NULL, &block,
				       IOCTL_DISK_GET_LENGTH_INFO,
				       NULL, 0, &length, sizeof(length));

	if (status == STATUS_SUCCESS) {
		*out_size = length.Length.QuadPart;
		co_debug("returned size: %llu KBs", (*out_size >> 10));
		return CO_RC(OK);
	}
	co_debug("fail status %X", (int)status);

	return CO_RC(ERROR);
}

static co_rc_t co_os_file_block_detect_size_standard(HANDLE handle, unsigned long long *out_size)
{
	NTSTATUS status;
	IO_STATUS_BLOCK IoStatusBlock;
	FILE_STANDARD_INFORMATION fsi;

	status = ZwQueryInformationFile(handle,
					&IoStatusBlock,
					&fsi,
					sizeof(fsi),
					FileStandardInformation);

	if (status == STATUS_SUCCESS) {
		*out_size = fsi.EndOfFile.QuadPart;
		co_debug("reported size: %llu KBs", (*out_size >> 10));
		return CO_RC(OK);
	} if (status != STATUS_INVALID_DEVICE_REQUEST)
		co_debug("fail status %X", (int)status);

	return CO_RC(ERROR);
}

co_rc_t co_os_file_get_size(HANDLE handle, unsigned long long *out_size)
{
	co_rc_t rc;

	rc = co_os_file_block_detect_size_standard(handle, out_size);
	if (CO_OK(rc))
		return rc;

	rc = co_os_file_block_detect_size_harddisk(handle, out_size);
	if (CO_OK(rc))
		return rc;

	/*
	 * Fall back to binary search.
	 */
	rc = co_os_file_block_detect_size_binary_search(handle, out_size);
	if (CO_OK(rc))
		return rc;

	*out_size = 0;
	return CO_RC(ERROR);
}

static co_rc_t co_os_file_block_get_size(co_monitor_file_block_dev_t *fdev, unsigned long long *size)
{
	HANDLE FileHandle;
	co_rc_t rc;

	if (fdev->sysdep != NULL)
		return CO_RC(ERROR);

	co_debug("device %s", fdev->pathname);
	rc = co_os_file_open(fdev->pathname, &FileHandle, FILE_READ_DATA);
	if (!CO_OK(rc))
		return rc;

	rc = co_os_file_get_size(FileHandle, size);

	co_os_file_close(FileHandle);

	return rc;
}

static co_rc_t co_os_file_block_open(co_monitor_t *linuxvm, co_monitor_file_block_dev_t *fdev)
{
	HANDLE *FileHandle = (HANDLE *)&fdev->sysdep;
	co_rc_t rc;

	/* Sync open */
	rc = co_os_file_open(fdev->pathname, FileHandle, FILE_READ_DATA | FILE_WRITE_DATA);
	if (CO_OK(rc))
		return CO_RC(OK);

	if (CO_RC_GET_CODE(rc) == CO_RC_ACCESS_DENIED) {
		co_rc_t rc2;
		/* try readonly */
		rc2 = co_os_file_open(fdev->pathname, FileHandle, FILE_READ_DATA);
		if (CO_OK(rc2))
			return CO_RC(OK);
	}

	return rc;
}

static co_rc_t co_os_file_block_async_open(co_monitor_t *linuxvm, co_monitor_file_block_dev_t *fdev)
{
	HANDLE *FileHandle = (HANDLE *)&fdev->sysdep;
	co_rc_t rc;

	/* Async open */
	rc = co_os_file_create(fdev->pathname, FileHandle, FILE_READ_DATA | FILE_WRITE_DATA, 0, FILE_OPEN, 0);
	if (CO_OK(rc))
		return CO_RC(OK);

	if (CO_RC_GET_CODE(rc) == CO_RC_ACCESS_DENIED) {
		co_rc_t rc2;
		/* try readonly */
		rc2 = co_os_file_create(fdev->pathname, FileHandle, FILE_READ_DATA, 0, FILE_OPEN, 0);
		if (CO_OK(rc2))
			return CO_RC(OK);
	}

	return rc;
}

static co_rc_t co_os_file_block_close(co_monitor_file_block_dev_t *fdev)
{
	HANDLE FileHandle;

	FileHandle = (HANDLE)(fdev->sysdep);

	co_os_file_close(FileHandle);

	fdev->sysdep = NULL;

	return CO_RC(OK);
}

co_monitor_file_block_operations_t co_os_file_block_async_operations = {
	.open = co_os_file_block_async_open,
	.close = co_os_file_block_close,
	.read = co_os_file_block_async_read,
	.write = co_os_file_block_async_write,
	.get_size = co_os_file_block_get_size,
};

co_monitor_file_block_operations_t co_os_file_block_default_operations = {
	.open = co_os_file_block_open,
	.close = co_os_file_block_close,
	.read = co_os_file_block_read,
	.write = co_os_file_block_write,
	.get_size = co_os_file_block_get_size,
};
