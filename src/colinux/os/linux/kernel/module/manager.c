/*
 * This source code is a part of coLinux source package.
 *
 * Dan Aloni <da-x@colinux.org>, 2003 (c)
 *
 * The code is licensed under the GPL. See the COPYING file at
 * the root directory.
 *
 */

#include "linux_inc.h"

#include "manager.h"

#include "../../ioctl.h"

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,26)
 #define CO_PROC_ROOT_PTR NULL
#else
 #define CO_PROC_ROOT_PTR &proc_root
#endif


static
int co_os_manager_open(struct inode *inode, struct file *file)
{
	co_manager_open_desc_t opened;
	co_rc_t rc;

	file->private_data = (void *)NULL;

        if (!try_module_get(THIS_MODULE))
		return -EBUSY;

	rc = co_manager_open(co_global_manager, &opened);
	if (!CO_OK(rc))
		return -ENOMEM;

	file->private_data = opened;

	return 0;
}

static
int co_os_manager_ioctl_buffer(co_linux_io_t *ioctl, char *buffer, struct file *file)
{
	co_rc_t rc;
	unsigned long return_size = 0;
	co_manager_open_desc_t opened = (typeof(opened))(file->private_data);

	if (copy_from_user(buffer, ioctl->input_buffer, ioctl->input_buffer_size))
		return -EFAULT;

	rc = co_manager_ioctl(co_global_manager,
			      ioctl->code,
			      buffer, ioctl->input_buffer_size,
			      ioctl->output_buffer_size, &return_size,
			      opened);
	if (!CO_OK(rc))
		return -EIO;

	if (return_size) {
		if (copy_to_user(ioctl->output_buffer, buffer, return_size))
			return -EFAULT;
	}

	if (ioctl->output_returned != NULL) {
		if (copy_to_user(ioctl->output_returned, &return_size, sizeof(unsigned long))) {
			return -EFAULT;
		}
	}

	return 0;
}

static
int co_os_manager_ioctl(struct inode *inode, struct file *file,
			unsigned int cmd, unsigned long arg)
{
	co_linux_io_t ioctl;
	unsigned long buffer_size;
	int ret = -1;

	if (cmd != CO_LINUX_IOCTL_ID)
		return -1;

	if (copy_from_user(&ioctl, (void *)arg, sizeof(ioctl)))
                return -EFAULT;

	buffer_size = ioctl.input_buffer_size;
	if (buffer_size < ioctl.output_buffer_size)
		buffer_size = ioctl.output_buffer_size;

	if (buffer_size > 0x400000)
		return -EIO;

	if (buffer_size > 80) {
		void *buffer = vmalloc(buffer_size);
		if (buffer == NULL) {
			    co_debug("ioctl buffer too big: %lx", buffer_size);
			    return -ENOMEM;
		}

		ret = co_os_manager_ioctl_buffer(&ioctl, buffer, file);
		vfree(buffer);
	} else {
		char on_stack[80];
		ret = co_os_manager_ioctl_buffer(&ioctl, on_stack, file);
	}

	return ret;
}

static
ssize_t co_os_manager_read(struct file *file, char __user *buffer, size_t size, loff_t *poffset)
{
	char __user *io_buffer_start;
	char __user *io_buffer;
	char __user *io_buffer_end;
	co_manager_open_desc_t opened = (typeof(opened))(file->private_data);
	int copied = 0;
	int ret = -EFAULT;
	co_queue_t *queue;
	co_rc_t rc;

	if (!opened)
		return -EIO;

	if (!opened->active)
		return -EIO;

	co_os_mutex_acquire(opened->lock);

	queue = &opened->out_queue;
	io_buffer_start = buffer;
	io_buffer = io_buffer_start;
	io_buffer_end = io_buffer + size;

	while (co_queue_size(queue) != 0)
	{
		co_message_queue_item_t *message_item;
		co_message_t *message;
		unsigned long size;

		rc = co_queue_peek_tail(queue, (void **)&message_item);
		if (!CO_OK(rc)) {
			ret = -EIO;
			break;
		}

		message = message_item->message;
		size = message->size + sizeof(*message);
		if (io_buffer + size > io_buffer_end) {
			break;
		}

		rc = co_queue_pop_tail(queue, (void **)&message_item);
		if (!CO_OK(rc))
			break;

		co_queue_free(queue, message_item);

		ret = copy_to_user(io_buffer, message, size);
		if (ret) {
			ret = -EFAULT;
			co_os_free(message);
			break;
		} else {
			ret = 0 ;
		}

		copied += size;
		io_buffer += size;
		co_os_free(message);
	}


	co_os_mutex_release(opened->lock);

	*poffset += copied;

	if (!ret)
		return copied;

	return ret;
}

static
ssize_t co_os_manager_write(struct file *file, const char __user *buffer, size_t size, loff_t *poffset)
{
	co_manager_open_desc_t opened = (typeof(opened))(file->private_data);
	const char __user *scan_buffer;
	co_message_t __user *message;
	unsigned long message_size;
	long size_left;
	long position;
	int ret = 0;

	if (!opened)
		return -EIO;

	if (!opened->active)
		return -EIO;

	if (!opened->monitor)
		return -EIO;

	scan_buffer = buffer;
	size_left = size;
	position = 0;

	while (size_left > 0) {
		message = (typeof(message))(&scan_buffer[position]);
		message_size = 0;
		ret = copy_from_user(&message_size, &message->size, sizeof(message->size));
		if (ret) {
			ret = -EFAULT;
			break;
		}

		message_size += sizeof(*message);
		size_left -= message_size;
		if (size_left >= 0) {
			char *kblock = kmalloc(message_size, GFP_KERNEL);
			if (kblock) {
				if (!copy_from_user(kblock, message, message_size)) {
					co_monitor_message_from_user_free(opened->monitor, (co_message_t *)kblock);
				} else {
					kfree(kblock);
				}
			}
		}

		position += message_size;
	}

	if (!ret)
		return position;

	return ret;
}

static
unsigned int co_os_manager_poll(struct file *file, struct poll_table_struct *pollts)
{
	co_manager_open_desc_t opened = (typeof(opened))(file->private_data);
        unsigned int mask = POLLOUT | POLLWRNORM, size;
	co_queue_t *queue;

	co_os_mutex_acquire(opened->lock);
	queue = &opened->out_queue;
	size = co_queue_size(queue);
	co_os_mutex_release(opened->lock);

        poll_wait(file, &opened->os->waitq, pollts);

        if (size)
                mask |= POLLIN | POLLRDNORM;

	if (!opened->active)
		mask |= POLLHUP;

        return mask;

}

static
int co_os_manager_release(struct inode *inode, struct file *file)
{
	co_manager_open_desc_t opened = (typeof(opened))(file->private_data);

	if (opened) {
		co_manager_open_desc_deactive_and_close(co_global_manager, opened);
		file->private_data = NULL;
	}

	module_put(THIS_MODULE);

	return 0;
}

static struct file_operations manager_fileops = {
        .open           = co_os_manager_open,
	.ioctl          = co_os_manager_ioctl,
	.read           = co_os_manager_read,
	.write          = co_os_manager_write,
	.poll           = co_os_manager_poll,
        .release        = co_os_manager_release,
};

co_rc_t co_os_manager_init(co_manager_t *manager, co_osdep_manager_t *osdep)
{
	co_rc_t rc = CO_RC(OK);
	co_osdep_manager_t dep;

	*osdep = dep = co_os_malloc(sizeof(*dep));
	if (dep == NULL)
		return CO_RC(OUT_OF_MEMORY);

	memset(dep, 0, sizeof(*dep));

	dep->proc_root = proc_mkdir("colinux", CO_PROC_ROOT_PTR);
	if (dep->proc_root == NULL) {
		rc = CO_RC(ERROR);
		goto error;
	}
	dep->proc_root->owner = THIS_MODULE;

	dep->proc_ioctl = create_proc_entry("ioctl",  S_IFREG|S_IRUSR|S_IWUSR, dep->proc_root);
	if (!dep->proc_ioctl) {
		rc = CO_RC(ERROR);
		goto error_root;
	}

	dep->proc_ioctl->proc_fops = &manager_fileops;
	dep->proc_ioctl->owner = THIS_MODULE;

	return rc;

error_root:
	remove_proc_entry("colinux", CO_PROC_ROOT_PTR);

error:
	co_os_free(dep);
	return rc;
}

void co_os_manager_free(co_osdep_manager_t osdep)
{
	remove_proc_entry("ioctl", osdep->proc_root);
	remove_proc_entry("colinux", CO_PROC_ROOT_PTR);
	co_os_free(osdep);
}


co_rc_t co_os_manager_userspace_open(co_manager_open_desc_t opened)
{
	opened->os = co_os_malloc(sizeof(*opened->os));
	if (!opened->os)
		return CO_RC(OUT_OF_MEMORY);

	memset(opened->os, 0, sizeof(*opened->os));
	init_waitqueue_head(&opened->os->waitq);

	return CO_RC(OK);
}

void co_os_manager_userspace_close(co_manager_open_desc_t opened)
{
	wake_up_interruptible(&opened->os->waitq);
}

bool_t co_os_manager_userspace_try_send_direct(
	co_manager_t *manager,
	co_manager_open_desc_t opened,
	co_message_t *message)
{
	wake_up_interruptible(&opened->os->waitq);
	return PFALSE;
}

co_rc_t co_os_manager_userspace_eof(co_manager_t *manager, co_manager_open_desc_t opened)
{
	wake_up_interruptible(&opened->os->waitq);
	return CO_RC(OK);
}

co_id_t co_os_current_id(void)
{
	return current->pid;
}

/*
 * Pin this thread to the processor it is on -- see colinux/os/kernel/misc.h.
 *
 * Not implemented for the Linux host yet, and deliberately not faked: the
 * x86-64 world switch is the code that needs this, it only runs on the Windows
 * host so far, and a silent no-op here would be a per-CPU state bug waiting for
 * whoever brings the Linux host up. set_cpus_allowed_ptr() is the equivalent
 * when that happens.
 */
void co_os_pin_cpu(void)
{
}

void co_os_unpin_cpu(void)
{
}

/*
 * Same story as co_os_pin_cpu above: refusing loudly beats no-opping
 * silently, because the caller is about to run a world switch that cannot
 * survive a migration.
 */
bool_t co_os_pin_cpu_to(unsigned long cpu)
{
	return PFALSE;
}

unsigned long co_os_cpu_count(void)
{
	return (unsigned long)num_online_cpus();
}

unsigned long co_os_current_cpu(void)
{
	return (unsigned long)smp_processor_id();
}
