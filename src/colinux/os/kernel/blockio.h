/*
 * This source code is a part of coLinux source package.
 *
 * The code is licensed under the GPL. See the COPYING file at
 * the root directory.
 */

#ifndef __CO_OS_KERNEL_BLOCKIO_H__
#define __CO_OS_KERNEL_BLOCKIO_H__

#include <colinux/common/common.h>

/*
 * Raw backing store for the cooperative block device.
 *
 * Deliberately separate from the co_os_file_* family in filesystem.h. Those
 * open and close a file per operation and take a co_monitor_t to translate
 * guest addresses through the pseudo-physical map. This is the other shape: a
 * handle held open for the life of the device, and a transfer straight into a
 * host virtual address, because guest physical is host physical here and the
 * caller has already resolved the guest's buffer to a mapping the driver owns.
 *
 * A backing object is a file or a raw partition; \\??\\C:\\root.img and
 * \\??\\\\PhysicalDrive0\\Partition2 both open the same way, so which one is
 * in use is a path, not a code path.
 */
typedef void* co_os_bdev_t;

extern co_rc_t co_os_bdev_open(const char* path, co_os_bdev_t* dev_out,
			       unsigned long long* size_out);
extern co_rc_t co_os_bdev_rw(co_os_bdev_t dev, unsigned long long offset,
			     void* buffer, unsigned long size, bool_t write);
extern void    co_os_bdev_close(co_os_bdev_t dev);

#endif
