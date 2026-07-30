#include <colinux/os/alloc.h>
#include <colinux/common/libc.h>
#include <colinux/os/kernel/filesystem.h>
#include <colinux/os/kernel/time.h>
#include <asm/errno.h>

#include "filesystem.h"
#include "monitor.h"
#include "transfer.h"

static struct co_filesystem_ops flat_mode; /* like UML's hostfs */
/* static struct co_filesystem_ops unix_attr_mode; like UML's humfs (TODO) */

static co_inode_t *ino_num_to_inode(int ino, co_filesystem_t *filesystem)
{
	co_inode_t *inode = NULL;

	if (ino == 1)
		return filesystem->root;

        co_list_each_entry(inode,
			   &filesystem->inode_hashes[ino % CO_FS_HASH_TABLE_SIZE],
			   hash_node)
	{
		if (inode->number == ino)
			goto out;
	}

	inode = NULL;

out:
	return inode;
}

static co_inode_t *alloc_inode(co_filesystem_t *filesystem, co_inode_t *parent, const char *name)
{
	co_inode_t *inode;

	inode = co_os_malloc(sizeof(*inode));
	if (!inode)
		return NULL;

	co_memset(inode, 0, sizeof(*inode));

	do {
		inode->number = filesystem->next_inode_num;
		filesystem->next_inode_num++;
	} while (ino_num_to_inode(inode->number, filesystem) != NULL);

	if (parent) {
		co_list_add_tail(&inode->node, &parent->sub_inodes);
		inode->parent = parent;
	}

	if (name != NULL) {
		int len = co_strlen(name);
		char *dup = co_os_malloc(len + 1);
		if (!dup) {
			co_list_del(&inode->node);
			co_os_free(inode);
			return NULL;
		}
		co_memcpy(dup, name, len+1);
		inode->name = dup;
	}

	co_list_init(&inode->sub_inodes);
	co_list_add_tail(&inode->flat_node, &filesystem->list_inodes);
	co_list_add_tail(&inode->hash_node, &filesystem->inode_hashes[inode->number % CO_FS_HASH_TABLE_SIZE]);

	filesystem->inodes_count++;
	co_debug_lvl(filesystem, 10, "inode [%d] allocated %p child '%s' of %p",
		     filesystem->inodes_count, inode, name ? name : "<root>", parent);

	return inode;
}

static void reparent_inode(co_inode_t *node, co_inode_t *new_parent)
{
	co_list_del(&node->node);
	co_list_add_head(&node->node, &new_parent->sub_inodes);
	node->parent = new_parent;
}

static co_inode_t *find_inode(co_filesystem_t *filesystem, co_inode_t *parent, const char *name)
{
	co_inode_t *inode = NULL;

        co_list_each_entry(inode, &parent->sub_inodes, node) {
		if (co_strcmp(inode->name, name) == 0)
			return inode;
	}

	return NULL;
}

static void free_inode(co_filesystem_t *filesystem, co_inode_t *inode)
{
	co_list_del(&inode->flat_node);
	co_list_del(&inode->hash_node);
	if (inode->parent)
		co_list_del(&inode->node);
	if (inode->names)
		co_filesystem_getdir_free(inode->names);
	if (inode->name)
		co_os_free(inode->name);
	co_os_free(inode);
	co_debug_lvl(filesystem, 10, "inode [%d] freed %p", filesystem->inodes_count, inode);
	filesystem->inodes_count--;
}

static void free_inode_children(co_filesystem_t *filesystem, co_list_t *delete_now,
				int level, co_list_t *delete_later)
{
	co_inode_t *inode, *inode_new;

	if (level > 5) {
		co_list_each_entry_safe(inode, inode_new, delete_now, node) {
			co_list_del(&inode->node);
			co_list_add_head(&inode->node, delete_later);
			inode->parent = NULL;
		}
	} else {
		co_list_each_entry_safe(inode, inode_new, delete_now, node) {
			free_inode_children(filesystem, &inode->sub_inodes, level + 1, delete_later);
			free_inode(filesystem, inode);
		}
	}
}

static void free_inode_with_children(co_filesystem_t *filesystem, co_inode_t *inode)
{
	co_list_t to_delete, to_delete_next;

	co_list_init(&to_delete);
	free_inode_children(filesystem, &inode->sub_inodes, 0, &to_delete);
	free_inode(filesystem, inode);

	while (!co_list_empty(&to_delete)) {
		co_list_init(&to_delete_next);
		free_inode_children(filesystem, &to_delete, 0, &to_delete_next);

		if (co_list_empty(&to_delete_next))
			break;

		co_list_init(&to_delete);
		free_inode_children(filesystem, &to_delete_next, 0, &to_delete);
	}
}

static co_rc_t inode_forget(co_filesystem_t *filesystem, co_inode_t *inode)
{
	if (inode)
		free_inode_with_children(filesystem, inode);

	return CO_RC(OK);
}

static co_rc_t inode_dir_open(co_filesystem_t *filesystem, co_inode_t *inode)
{
	co_rc_t rc;

	if (!inode)
		return CO_RC(ERROR);

	if (inode->names) {
		inode->names->refcount++;
		return CO_RC(OK);
	}

	inode->names = co_os_malloc(sizeof(co_filesystem_dir_names_t));
	if (!inode->names)
		return CO_RC(OUT_OF_MEMORY);

	inode->names->refcount = 1;
	co_list_init(&inode->names->list);

	rc = filesystem->ops->getdir(filesystem, inode, inode->names);

	if (!CO_OK(rc)) {
		co_os_free(inode->names);
		inode->names = NULL;
	}

	return rc;
}

static co_rc_t inode_open(co_filesystem_t *filesystem, co_inode_t *inode, unsigned long flags)
{
	return CO_RC(OK);
}

static co_rc_t inode_read(co_monitor_t *cmon, co_filesystem_t *filesystem, co_inode_t *inode,
			  unsigned long long offset, unsigned long size,
			  vm_ptr_t dest_buffer)
{
	return filesystem->ops->inode_read_write(cmon, filesystem, inode, offset, size, dest_buffer, PTRUE);
}

static co_rc_t inode_write(co_monitor_t *cmon, co_filesystem_t *filesystem, co_inode_t *inode,
			   unsigned long long offset, unsigned long size,
			   vm_ptr_t src_buffer)
{
	return filesystem->ops->inode_read_write(cmon, filesystem, inode, offset, size, src_buffer, PFALSE);
}

static co_rc_t inode_mknod(co_filesystem_t *filesystem, co_inode_t *dir, unsigned long mode,
			   unsigned long rdev, char *name, int *ino, struct fuse_attr *attr)
{
	co_rc_t rc;

	attr->size = 0;
	attr->mode = mode & filesystem->file_mode;
	attr->nlink = 1;
	attr->uid = filesystem->uid;
	attr->gid = filesystem->gid;
	attr->rdev = 0;
	attr->_dummy = 0;
	attr->blocks = 0;
	attr->atime = \
	attr->mtime = \
	attr->ctime = co_os_get_time();

	rc = filesystem->ops->inode_mknod(filesystem, dir, mode, rdev, name, ino, attr);

	if (CO_OK(rc)) {
		co_inode_t *inode;

		inode = find_inode(filesystem, dir, name);
		if (!inode)
			inode = alloc_inode(filesystem, dir, name);
		*ino = inode->number;
	}

	return rc;
}

static co_rc_t inode_mkdir(co_filesystem_t *filesystem, co_inode_t *inode, unsigned long mode,
			   char *name)
{
	return filesystem->ops->inode_mkdir(filesystem, inode, mode, name);
}

static co_rc_t inode_unlink(co_filesystem_t *filesystem, co_inode_t *inode, char *name)
{
	return filesystem->ops->inode_unlink(filesystem, inode, name);
}

static co_rc_t inode_rmdir(co_filesystem_t *filesystem, co_inode_t *inode, char *name)
{
	return filesystem->ops->inode_rmdir(filesystem, inode, name);
}

static co_rc_t inode_set_attr(co_filesystem_t *filesystem, co_inode_t *inode,
			      unsigned long valid, struct fuse_attr *attr)
{
	return filesystem->ops->inode_set_attr(filesystem, inode, valid, attr);
}

static co_rc_t inode_rename(co_filesystem_t *filesystem, co_inode_t *dir,
			    int new_dir_num, char *oldname, char *newname)
{
	static co_inode_t *new_dir_inode;
	co_rc_t rc;

	new_dir_inode = ino_num_to_inode(new_dir_num, filesystem);
	if (!new_dir_inode)
		return CO_RC(ERROR);
	rc = filesystem->ops->inode_rename(filesystem, dir, new_dir_inode, oldname, newname);
	if (CO_OK(rc)) {
		co_inode_t *old_inode = find_inode(filesystem, dir, oldname);
		if (old_inode) {
			int size;

			// This moves the file from one dir to an other
			reparent_inode(old_inode, new_dir_inode);

			// This renames the file on the same inode number
			co_os_free(old_inode->name);
			size = co_strlen(newname) + 1;
			old_inode->name = co_os_malloc(size);
			if (!old_inode->name)
				return CO_RC(OUT_OF_MEMORY);
			co_memcpy(old_inode->name, newname, size);
		}
	}
	return rc;
}


static co_rc_t inode_dir_read(co_monitor_t *cmon,
			      co_inode_t *inode,
			      vm_ptr_t buff,
			      unsigned long size,
			      co_param_t *fill_size,
			      unsigned long file_pos)
{
	co_filesystem_name_t *name;
	unsigned long file_pos_seek = 0;
	struct fuse_dirent dirent;
	unsigned long dirent_size;
	vm_ptr_t buff_writeptr;
	co_rc_t rc;

	if (!inode)
		return CO_RC(ERROR);

	if (!inode->names)
		return CO_RC(ERROR);

	buff_writeptr = buff;
	*fill_size = 0;

        co_list_each_entry(name, &inode->names->list, node) {
		int slen = co_strlen(name->name);

		dirent_size = FUSE_DIRENT_ALIGN(FUSE_NAME_OFFSET + slen);
		if (file_pos_seek < file_pos) {
			file_pos_seek += dirent_size;
			continue;
		}

		if (dirent_size + *fill_size > size) {
			/* No space left in buffer */
			break;
		}

		if (co_strcmp(name->name, "..") == 0) {
			if (inode->parent)
				dirent.ino = inode->parent->number;
			else
				dirent.ino = inode->number;
		} else if (co_strcmp(name->name, ".") == 0) {
			dirent.ino = inode->number;
		} else {
			dirent.ino = -1;
		}

		/* TODO: Make it dynamicly.  See NAME_MAX in linux kernel. */
		if (slen > sizeof(dirent.name)) {
			/* Name to long */
			co_debug_lvl(filesystem, 5, "name to long (%d) '%s'", slen, name->name);
			slen = sizeof(dirent.name);
		}

		dirent.namelen = slen;
		dirent.type = name->type;

		co_memcpy(dirent.name, name->name, slen);

		rc = co_monitor_host_to_linuxvm(cmon, &dirent, buff_writeptr, dirent_size);
		if (!CO_OK(rc))
			return rc;

		buff_writeptr += dirent_size;
		*fill_size += dirent_size;
		file_pos_seek += dirent_size;
	}

	return CO_RC(OK);
}

static co_rc_t inode_dir_release(co_inode_t *inode)
{
	if (!inode) {
		return CO_RC(ERROR);
	}

	if (!inode->names)
		return CO_RC(ERROR);

	inode->names->refcount--;
	if (inode->names->refcount > 0)
		return CO_RC(OK);

	co_filesystem_getdir_free(inode->names);
	inode->names = NULL;

	return CO_RC(OK);
}

void co_filesystem_getdir_free(co_filesystem_dir_names_t *names)
{
	co_filesystem_name_t *name;

	while (!co_list_empty(&names->list)) {
		co_list_entry_assign(names->list.next, name, node);
		co_list_del(&name->node);
		co_os_free(name);
	}
}

co_rc_t co_monitor_file_system_init(co_monitor_t *cmon, unsigned int unit,
				    co_cofsdev_desc_t *desc)
{
	co_filesystem_t *filesystem;
	int i;

	filesystem = co_os_malloc(sizeof(*filesystem));
	if (!filesystem)
		return CO_RC(OUT_OF_MEMORY);

	co_memset(filesystem, 0, sizeof(*filesystem));

	for (i=0; i < CO_FS_HASH_TABLE_SIZE; i++) {
		co_list_init(&filesystem->inode_hashes[i]);
	}

	filesystem->next_inode_num = 1;

	co_list_init(&filesystem->list_inodes);
	co_memcpy(&filesystem->base_path, &desc->pathname, sizeof(co_pathname_t));
	filesystem->desc = desc;
	filesystem->ops = &flat_mode; /* The only supported mode at the moment */
	filesystem->root = alloc_inode(filesystem, NULL, NULL);

	if (!filesystem->root) {
		co_os_free(filesystem);
		return CO_RC(OUT_OF_MEMORY);
	}

	cmon->filesystems[unit] = filesystem;

	return CO_RC(OK);
}

void co_monitor_file_system_free(co_monitor_t *cmon, int unit)
{
	co_filesystem_t *filesystem = cmon->filesystems[unit];
	co_inode_t *inode;

	if (!filesystem)
		return;

	while (!co_list_empty(&filesystem->list_inodes)) {
		co_list_entry_assign(filesystem->list_inodes.next, inode, flat_node);
		free_inode(filesystem, inode);
	}

	co_os_free(filesystem);
	cmon->filesystems[unit] = NULL;
}

static co_rc_t inode_get_attr(co_filesystem_t *filesystem, co_inode_t *inode,
			      struct fuse_getattr_out *attr)
{
	char *name;

	name = inode->name;
	inode = inode->parent;

	if (name == NULL)
		name = "";

	return filesystem->ops->getattr(filesystem, inode, name, &attr->attr);
}

static co_rc_t inode_lookup(co_filesystem_t *filesystem, co_inode_t *dir,
			    char *name, struct fuse_lookup_out *args)
{
	co_rc_t rc = filesystem->ops->getattr(filesystem, dir, name, &args->attr);

	if (CO_OK(rc)) {
		co_inode_t *inode = NULL;
		inode = find_inode(filesystem, dir, name);
		if (!inode)
			inode = alloc_inode(filesystem, dir, name);
		args->ino = inode->number;
	}

	return rc;
}

static int translate_code(co_rc_t value)
{
	switch (CO_RC_GET_CODE(value)) {
	case CO_RC_NOT_FOUND:
		return -ENOENT;
	case CO_RC_ACCESS_DENIED:
		return -EPERM;
	case CO_RC_INVALID_PARAMETER:
		return -EINVAL;
	case 0:
		return 0;
	default:
		return -EIO;
	}
}

static co_rc_t fs_mount(co_filesystem_t *filesystem, const char *pathname,
			int uid, int gid,
			unsigned long dir_mode, unsigned long file_mode,
			int flags)
{
	co_cofsdev_desc_t *desc;
	co_rc_t rc;

	desc = filesystem->desc;

	filesystem->uid = uid;
	filesystem->gid = gid;
	filesystem->dir_mode = dir_mode;
	filesystem->file_mode = file_mode;
	filesystem->flags = flags;

	co_memcpy(&filesystem->base_path, &desc->pathname, sizeof(co_pathname_t));

	rc = co_os_fs_dir_join_unix_path(&filesystem->base_path, pathname);

	return rc;
}

static co_rc_t fs_stat(co_filesystem_t *filesystem, struct fuse_statfs_out *statfs)
{
	return filesystem->ops->fs_stat(filesystem, statfs);
}

void co_monitor_file_system(co_monitor_t *cmon, unsigned int unit,
			    enum fuse_opcode opcode, co_param_t *params)
{
	int ino = -1;
	co_filesystem_t *filesystem;
	co_inode_t *inode;
	int result = 0;

	filesystem = cmon->filesystems[unit];
	if (!filesystem) {
		result = -ENODEV;
		goto out;
	}

	switch (opcode) {
	case FUSE_MOUNT:
		result = fs_mount(filesystem, (char*)(&co_passage_page->params[30]),
				  co_passage_page->params[5],
				  co_passage_page->params[6],
				  co_passage_page->params[7],
				  co_passage_page->params[8],
				  co_passage_page->params[9]);
		result = translate_code(result);
		goto out;
	case FUSE_STATFS:
		result = fs_stat(filesystem, (struct fuse_statfs_out *)(&co_passage_page->params[5]));
		result = translate_code(result);
		goto out;
	default:
		break;
	}

	ino = params[0];
	inode = ino_num_to_inode(ino, filesystem);

	switch (opcode) {
	case FUSE_SETATTR: {
		result = inode_set_attr(filesystem, inode,
					co_passage_page->params[5],
					(struct fuse_attr *)(&co_passage_page->params[6]));
		result = translate_code(result);
		break;
	}

	case FUSE_RENAME: {
		char *str = (char *)&co_passage_page->params[30];
		result = inode_rename(filesystem, inode,
				      co_passage_page->params[5],
				      str,
				      str + co_strlen(str) + 1);
		result = translate_code(result);
		break;
	}

	case FUSE_FORGET: {
		inode_forget(filesystem, inode);
		break;
	}

	case FUSE_MKNOD:
		result = inode_mknod(filesystem, inode,
				     co_passage_page->params[5],
				     co_passage_page->params[6],
				     (char *)&co_passage_page->params[30],
				     (int *)&co_passage_page->params[7],
				     (struct fuse_attr *)(&co_passage_page->params[8]));
		result = translate_code(result);
		break;

	case FUSE_MKDIR:
		result = inode_mkdir(filesystem, inode,
				      co_passage_page->params[5],
				      (char *)&co_passage_page->params[30]);
		result = translate_code(result);
		break;

	case FUSE_UNLINK:
		result = inode_unlink(filesystem, inode,
				      (char *)&co_passage_page->params[30]);
		result = translate_code(result);
		break;

	case FUSE_RMDIR:
		result = inode_rmdir(filesystem, inode,
				     (char *)&co_passage_page->params[30]);
		result = translate_code(result);
		break;

	case FUSE_WRITE: {
		result = inode_write(cmon, filesystem, inode,
				     *((unsigned long long *)&co_passage_page->params[5]),
				     co_passage_page->params[7],
				     co_passage_page->params[8]);
		result = translate_code(result);
		break;
	}

	case FUSE_READ: {
		result = inode_read(cmon, filesystem, inode,
				    *((unsigned long long *)&co_passage_page->params[5]),
				    co_passage_page->params[7],
				    co_passage_page->params[8]);
		result = translate_code(result);
		break;
	}

	case FUSE_OPEN: {
		result = inode_open(filesystem, inode, co_passage_page->params[5]);
		result = translate_code(result);
		break;
	}

	case FUSE_LOOKUP: {
		result =
			inode_lookup(filesystem, inode,
				     (char *)&co_passage_page->params[30],
				     (struct fuse_lookup_out *)&co_passage_page->params[5]);
		result = translate_code(result);
		break;
	}
	case FUSE_GETATTR: {
		result = inode_get_attr(filesystem, inode,
					(struct fuse_getattr_out *)&co_passage_page->params[5]);
		result = translate_code(result);
		break;
	}

	case FUSE_DIR_OPEN:
		result = inode_dir_open(filesystem, inode);
		result = translate_code(result);
		break;

	case FUSE_DIR_READ:
		result = inode_dir_read(cmon, inode,
					co_passage_page->params[6],
					co_passage_page->params[5],
					&co_passage_page->params[7],
					co_passage_page->params[8]);
		result = translate_code(result);
		break;

	case FUSE_DIR_RELEASE:
		result = inode_dir_release(inode);
		result = translate_code(result);
		break;

	case FUSE_GETDIR:
		break;
	default:
		break;
	}

out:
	co_passage_page->params[4] = result;
}

void co_monitor_unregister_filesystems(co_monitor_t *cmon)
{
	int i;

	for (i=0; i < CO_MODULE_MAX_COFS; i++)
		co_monitor_file_system_free(cmon, i);
}


/*
 *  Flat mode implementation.
 */

static co_rc_t co_fs_get_attr(co_filesystem_t *fs, char *filename, struct fuse_attr *attr)
{
	co_rc_t rc;

	rc = co_os_file_get_attr(filename, attr);
	if (!CO_OK(rc))
		return rc;

	if (fs->flags & COFS_MOUNT_NOATTRIB)
		if (attr->mode & FUSE_S_IFDIR)
			attr->mode = fs->dir_mode;
		else
			attr->mode = fs->file_mode;
	else
		if (attr->mode & FUSE_S_IFDIR)
			attr->mode &= fs->dir_mode;
		else
			attr->mode &= fs->file_mode;

	attr->uid = fs->uid;
	attr->gid = fs->gid;

	return rc;
}

static co_rc_t flat_mode_inode_rename(co_filesystem_t *filesystem, co_inode_t *old_inode, co_inode_t *new_inode,
			     char *oldname, char *newname)
{
	char *old_dirname = NULL, *new_dirname = NULL;
	co_rc_t rc;

	rc = co_os_fs_dir_inode_to_path(filesystem, old_inode, &old_dirname, oldname);
	if (CO_OK(rc)) {
		rc = co_os_fs_dir_inode_to_path(filesystem, new_inode, &new_dirname, newname);
		if (CO_OK(rc)) {
			rc = co_os_file_rename(old_dirname, new_dirname);
			co_os_free(new_dirname);
		}
		co_os_free(old_dirname);
	}

	return rc;
}

static co_rc_t flat_mode_getattr(co_filesystem_t *fs, co_inode_t *dir,
				 char *name, struct fuse_attr *attr)
{
	char *filename;
	co_rc_t rc;

	rc = co_os_fs_dir_inode_to_path(fs, dir, &filename, name);
	if (!CO_OK(rc))
		return rc;

	rc = co_fs_get_attr(fs, filename, attr);
	co_os_free(filename);

	return rc;
}

static co_rc_t flat_mode_getdir(co_filesystem_t *fs, co_inode_t *dir, co_filesystem_dir_names_t *names)
{
	char *dirname;
	co_rc_t rc;

	rc = co_os_fs_inode_to_path(fs, dir, &dirname, 1);
	if (!CO_OK(rc))
		return rc;

	rc = co_os_file_getdir(dirname, names);
	co_os_free(dirname);

	return rc;
}

static co_rc_t flat_mode_inode_read_write(co_monitor_t *linuxvm, co_filesystem_t *filesystem, co_inode_t *inode,
				  unsigned long long offset, unsigned long size,
				  vm_ptr_t src_buffer, bool_t read)
{
	char *filename;
	co_rc_t rc;

	rc = co_os_fs_inode_to_path(filesystem, inode, &filename, 0);
	if (!CO_OK(rc))
		return rc;

	rc = co_os_file_read_write(linuxvm, filename, offset, size, src_buffer, read);
	co_os_free(filename);

	return rc;
}

static co_rc_t flat_mode_inode_mknod(co_filesystem_t *filesystem, co_inode_t *inode, unsigned long mode,
			     unsigned long rdev, char *name, int *ino, struct fuse_attr *attr)
{
	char *filename;
	co_rc_t rc;

	rc = co_os_fs_dir_inode_to_path(filesystem, inode, &filename, name);
	if (!CO_OK(rc))
		return rc;

	rc = co_os_file_mknod(filesystem, filename, mode);
	co_os_free(filename);

	return rc;
}

static co_rc_t flat_mode_inode_set_attr(co_filesystem_t *filesystem, co_inode_t *inode,
				unsigned long valid, struct fuse_attr *attr)
{
	char *filename;
	co_rc_t rc;

	rc = co_os_fs_inode_to_path(filesystem, inode, &filename, 0);
	if (!CO_OK(rc))
		return rc;

	if (filesystem->flags & COFS_MOUNT_NOATTRIB)
		valid &= ~FATTR_MODE;

	rc = co_os_file_set_attr(filename, valid, attr);
	co_os_free(filename);

	return rc;
}

static co_rc_t flat_mode_inode_mkdir(co_filesystem_t *filesystem, co_inode_t *inode, unsigned long mode,
			     char *name)
{
	char *dirname;
	co_rc_t rc;

	rc = co_os_fs_dir_inode_to_path(filesystem, inode, &dirname, name);
	if (!CO_OK(rc))
		return rc;

	rc = co_os_file_mkdir(dirname);
	co_os_free(dirname);

	return rc;
}

static co_rc_t flat_mode_inode_unlink(co_filesystem_t *filesystem, co_inode_t *inode, char *name)
{
	char *filename;
	co_rc_t rc;

	rc = co_os_fs_dir_inode_to_path(filesystem, inode, &filename, name);
	if (!CO_OK(rc))
		return rc;

	rc = co_os_file_unlink(filename);
	co_os_free(filename);

	return rc;
}

static co_rc_t flat_mode_inode_rmdir(co_filesystem_t *filesystem, co_inode_t *inode, char *name)
{
	char *dirname;
	co_rc_t rc;

	rc = co_os_fs_dir_inode_to_path(filesystem, inode, &dirname, name);
	if (!CO_OK(rc))
		return rc;

	rc = co_os_file_rmdir(dirname);
	co_os_free(dirname);

	return rc;
}


static co_rc_t flat_mode_fs_stat(co_filesystem_t *filesystem, struct fuse_statfs_out *statfs)
{
	return co_os_file_fs_stat(filesystem, statfs);
}

static struct co_filesystem_ops flat_mode = {
	.inode_rename = flat_mode_inode_rename,
	.getattr = flat_mode_getattr,
	.getdir = flat_mode_getdir,
	.inode_read_write = flat_mode_inode_read_write,
	.inode_mknod = flat_mode_inode_mknod,
	.inode_set_attr = flat_mode_inode_set_attr,
	.inode_mkdir = flat_mode_inode_mkdir,
	.inode_unlink = flat_mode_inode_unlink,
	.inode_rmdir = flat_mode_inode_rmdir,
	.fs_stat = flat_mode_fs_stat,
};
