/*
 * This source code is a part of coLinux source package.
 *
 * Dan Aloni <da-x@colinux.org>, 2004 (c)
 *
 * The code is licensed under the GPL. See the COPYING file at
 * the root directory.
 */

#ifndef __COLINUX_KERNEL_FILE_SYSTEM_H__
#define __COLINUX_KERNEL_FILE_SYSTEM_H__

#include <colinux/common/list.h>
#include <colinux/common/common.h>
#include <colinux/common/config.h>

#include <linux/cooperative_fs.h>

typedef struct  co_filesystem_name {
	co_list_t node;
	unsigned char type;
	char name[1];
} co_filesystem_name_t;

typedef struct co_filesystem_dir_names {
	co_list_t list;
	int refcount;
} co_filesystem_dir_names_t;

typedef struct co_inode {
	co_list_t flat_node;
	co_list_t hash_node;
	co_list_t node;
	struct co_inode *parent;
	co_list_t sub_inodes;
	char *name;
	co_filesystem_dir_names_t *names;
	int number;
} co_inode_t;

#define CO_FS_HASH_TABLE_SIZE     0x1000

typedef struct co_filesystem {
	co_list_t list_inodes;
	co_cofsdev_desc_t *desc;
	co_pathname_t base_path;
	co_inode_t *root;
	int inodes_count;
	int uid;
	int gid;
	int dir_mode;
	int file_mode;
	int flags;
	struct co_filesystem_ops *ops;

	/* Inode hash table */
	co_list_t inode_hashes[CO_FS_HASH_TABLE_SIZE];
	int next_inode_num;
} co_filesystem_t;

struct co_monitor;

typedef struct co_filesystem_ops {
	co_rc_t (*inode_rename)(co_filesystem_t *filesystem, co_inode_t *dir, co_inode_t *new_dir,
				char *oldname, char *newname);
	co_rc_t (*getattr)(co_filesystem_t *fs, co_inode_t *dir, char *name, struct fuse_attr *attr);
	co_rc_t (*getdir)(co_filesystem_t *fs, co_inode_t *dir, co_filesystem_dir_names_t *names);
	co_rc_t (*inode_read_write)(struct co_monitor *linuxvm, co_filesystem_t *filesystem,
				    co_inode_t *inode, unsigned long long offset, unsigned long size,
				    vm_ptr_t src_buffer, bool_t read);
	co_rc_t (*inode_mknod)(co_filesystem_t *filesystem, co_inode_t *inode, unsigned long mode,
			       unsigned long rdev, char *name, int *ino, struct fuse_attr *attr);
	co_rc_t (*inode_set_attr)(co_filesystem_t *filesystem, co_inode_t *inode,
				  unsigned long valid, struct fuse_attr *attr);
	co_rc_t (*inode_mkdir)(co_filesystem_t *filesystem, co_inode_t *inode,
			       unsigned long mode, char *name);
	co_rc_t (*inode_unlink)(co_filesystem_t *filesystem, co_inode_t *inode, char *name);
	co_rc_t (*inode_rmdir)(co_filesystem_t *filesystem, co_inode_t *inode, char *name);
	co_rc_t (*fs_stat)(co_filesystem_t *filesystem, struct fuse_statfs_out *statfs);
} co_filesystem_ops_t;

struct co_monitor;
extern void co_monitor_file_system(struct co_monitor *cmon, unsigned int unit,
				   enum fuse_opcode opcode, co_param_t *params);

extern void co_filesystem_getdir_free(co_filesystem_dir_names_t *names);

extern co_rc_t co_monitor_file_system_init(struct co_monitor *cmon, unsigned int unit,
					   co_cofsdev_desc_t *desc);

extern void co_monitor_unregister_filesystems(struct co_monitor *cmon);

#endif
