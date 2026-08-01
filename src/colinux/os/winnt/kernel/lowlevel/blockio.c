/*
 * This source code is a part of coLinux source package.
 *
 * The code is licensed under the GPL. See the COPYING file at
 * the root directory.
 */

#include "../ddk.h"

#include <colinux/common/common.h>
#include <colinux/common/libc.h>
#include <colinux/common/debug.h>
#include <colinux/os/alloc.h>
#include <colinux/os/kernel/alloc.h>
#include <colinux/os/kernel/blockio.h>

typedef struct {
	HANDLE		   handle;
	unsigned long long size;
} co_winnt_bdev_t;

/*
 * Declared here because mingw-w64's kernel-mode headers carry the disk IOCTLs
 * in ntddstor.h, which cannot be included alongside the DDK headers this
 * driver already has. Both are ABI, fixed since NT 4: the control code is
 * CTL_CODE(IOCTL_DISK_BASE, 0x17, METHOD_BUFFERED, FILE_READ_ACCESS).
 */
#define CO_IOCTL_DISK_GET_LENGTH_INFO	0x0007405C

typedef struct {
	LARGE_INTEGER Length;
} co_get_length_information_t;

extern co_rc_t co_winnt_utf8_to_unicode(const char *src, UNICODE_STRING *unicode_str);
extern void co_winnt_free_unicode(UNICODE_STRING *unicode_str);

/*
 * The size of the backing object.
 *
 * A file answers FileStandardInformation. A raw partition does not -- it is
 * not a file system object and reports zero length -- so it is asked with the
 * disk IOCTL instead. Trying both in this order means one path serves an image
 * file and a real partition without the caller knowing which it has.
 */
static co_rc_t co_winnt_bdev_size(HANDLE handle, unsigned long long* size_out)
{
	IO_STATUS_BLOCK	 iosb;
	FILE_STANDARD_INFORMATION info;
	co_get_length_information_t length;
	NTSTATUS status;

	status = ZwQueryInformationFile(handle, &iosb, &info, sizeof(info),
					FileStandardInformation);
	if (NT_SUCCESS(status) && info.EndOfFile.QuadPart > 0) {
		*size_out = (unsigned long long)info.EndOfFile.QuadPart;
		return CO_RC(OK);
	}

	status = ZwDeviceIoControlFile(handle, NULL, NULL, NULL, &iosb,
				       CO_IOCTL_DISK_GET_LENGTH_INFO,
				       NULL, 0, &length, sizeof(length));
	if (NT_SUCCESS(status) && length.Length.QuadPart > 0) {
		*size_out = (unsigned long long)length.Length.QuadPart;
		return CO_RC(OK);
	}

	co_debug_error("bdev: cannot determine size (status %x)", (int)status);

	return CO_RC(ERROR);
}

co_rc_t co_os_bdev_open(const char* path, co_os_bdev_t* dev_out,
			unsigned long long* size_out)
{
	OBJECT_ATTRIBUTES attributes;
	IO_STATUS_BLOCK	  iosb;
	UNICODE_STRING	  unipath;
	co_winnt_bdev_t*  dev;
	NTSTATUS	  status;
	co_rc_t		  rc;

	*dev_out = NULL;

	dev = co_os_malloc(sizeof(*dev));
	if (!dev)
		return CO_RC(OUT_OF_MEMORY);

	co_memset(dev, 0, sizeof(*dev));

	rc = co_winnt_utf8_to_unicode(path, &unipath);
	if (!CO_OK(rc)) {
		co_os_free(dev);
		return rc;
	}

	InitializeObjectAttributes(&attributes, &unipath,
				   OBJ_CASE_INSENSITIVE | OBJ_KERNEL_HANDLE,
				   NULL, NULL);

	/*
	 * Opened without write sharing on purpose. If Windows still has this
	 * volume mounted, or another instance has the image open, the open
	 * fails here rather than the two of them writing over one another --
	 * which for a mounted NTFS volume would destroy the host's filesystem,
	 * and for an image would corrupt it in a way that reads back as a
	 * kernel bug.
	 */
	status = ZwCreateFile(&dev->handle,
			      FILE_READ_DATA | FILE_WRITE_DATA | SYNCHRONIZE,
			      &attributes, &iosb, NULL, FILE_ATTRIBUTE_NORMAL,
			      FILE_SHARE_READ, FILE_OPEN,
			      FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE,
			      NULL, 0);

	co_winnt_free_unicode(&unipath);

	if (!NT_SUCCESS(status)) {
		co_debug_error("bdev: cannot open '%s' (status %x)", path, (int)status);
		co_os_free(dev);
		return CO_RC(ERROR);
	}

	rc = co_winnt_bdev_size(dev->handle, &dev->size);
	if (!CO_OK(rc)) {
		ZwClose(dev->handle);
		co_os_free(dev);
		return rc;
	}

	*dev_out  = dev;
	*size_out = dev->size;

	return CO_RC(OK);
}

co_rc_t co_os_bdev_rw(co_os_bdev_t handle, unsigned long long offset,
		      void* buffer, unsigned long size, bool_t write)
{
	co_winnt_bdev_t* dev = (co_winnt_bdev_t*)handle;
	IO_STATUS_BLOCK	 iosb;
	LARGE_INTEGER	 where;
	NTSTATUS	 status;

	if (!dev || !dev->handle)
		return CO_RC(ERROR);

	/*
	 * Refused rather than clamped. A transfer past the end is the guest
	 * asking for something that is not there, which means its idea of the
	 * device's size disagrees with ours -- and a short read that reports
	 * success hands the filesystem a buffer of stale bytes and lets it
	 * carry on believing them.
	 */
	if (offset > dev->size || size > dev->size - offset) {
		co_debug_error("bdev: %s past end -- offset %llu size %lu of %llu",
			       write ? "write" : "read", offset, size, dev->size);
		return CO_RC(ERROR);
	}

	where.QuadPart = (LONGLONG)offset;

	if (write)
		status = ZwWriteFile(dev->handle, NULL, NULL, NULL, &iosb,
				     buffer, size, &where, NULL);
	else
		status = ZwReadFile(dev->handle, NULL, NULL, NULL, &iosb,
				    buffer, size, &where, NULL);

	if (!NT_SUCCESS(status)) {
		co_debug_error("bdev: %s failed at %llu+%lu (status %x)",
			       write ? "write" : "read", offset, size, (int)status);
		return CO_RC(ERROR);
	}

	if (iosb.Information != size) {
		co_debug_error("bdev: short %s at %llu -- %lu of %lu",
			       write ? "write" : "read", offset,
			       (unsigned long)iosb.Information, size);
		return CO_RC(ERROR);
	}

	return CO_RC(OK);
}

void co_os_bdev_close(co_os_bdev_t handle)
{
	co_winnt_bdev_t* dev = (co_winnt_bdev_t*)handle;

	if (!dev)
		return;

	if (dev->handle)
		ZwClose(dev->handle);

	co_os_free(dev);
}
