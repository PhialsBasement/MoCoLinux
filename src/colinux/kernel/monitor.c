/*
 * This source code is a part of coLinux source package.
 *
 * Dan Aloni <da-x@colinux.org>, 2003-2004 (c)
 *
 * The code is licensed under the GPL. See the COPYING file at
 * the root directory.
 */

/*
 * Monitor
 *
 * FIXME: It should be architecture independant, but I am not sure if it
 * currently achieves that goal. Maybe with a few fixes...
 */

#include <colinux/common/debug.h>
#include <colinux/common/libc.h>
#include <colinux/os/kernel/alloc.h>
#include <colinux/os/kernel/misc.h>
#include <colinux/os/kernel/monitor.h>
#include <colinux/os/kernel/time.h>
#include <colinux/os/kernel/wait.h>
#include <colinux/os/timer.h>
#include <colinux/arch/passage.h>
#include <colinux/arch/interrupt.h>
#include <colinux/arch/mmu.h>

#include "monitor.h"
#include "manager.h"
#include "scsi.h"
#include "block.h"
#include "fileblock.h"
#include "transfer.h"
#include "filesystem.h"
#include "pages.h"
#include "pci.h"
#include "video.h"

#define co_offsetof(TYPE, MEMBER) ((int) &((TYPE *)0)->MEMBER)

co_rc_t co_monitor_malloc(co_monitor_t* cmon, unsigned long bytes, void** ptr)
{
	void* block = co_os_malloc(bytes);

	if (block == NULL)
		return CO_RC(OUT_OF_MEMORY);

	*ptr = block;

	cmon->blocks_allocated++;

	return CO_RC(OK);
}

co_rc_t co_monitor_free(co_monitor_t* cmon, void* ptr)
{
	co_os_free(ptr);

	cmon->blocks_allocated--;

	return CO_RC(OK);
}

static co_rc_t guest_address_space_init(co_monitor_t *cmon)
{
	co_rc_t		rc   = CO_RC(OK);
	co_pfn_t*	pfns = NULL;
	co_pfn_t 	self_map_pfn;
	co_pfn_t 	passage_page_pfn;
	co_pfn_t 	swapper_pg_dir_pfn;
	unsigned long 	pp_pagetables_pgd;
	unsigned long	self_map_page_offset;
	unsigned long 	passage_page_offset;
	unsigned long	reversed_physical_mapping_offset;

	rc = co_monitor_get_pfn(cmon, cmon->import.kernel_swapper_pg_dir, &swapper_pg_dir_pfn);
	if (!CO_OK(rc)) {
		co_debug_error("error %08x getting swapper_pg_dir pfn", (int)rc);
		goto out_error;
	}

	cmon->pgd = swapper_pg_dir_pfn << CO_ARCH_PAGE_SHIFT;

	rc = co_monitor_arch_passage_page_alloc(cmon);
	if (!CO_OK(rc)) {
		co_debug_error("error %08x allocating passage page", (int)rc);
		goto out_error;
	}

	rc = co_monitor_arch_passage_page_init(cmon);
	if (!CO_OK(rc)) {
		co_debug_error("error %08x initializing the passage page", (int)rc);
		goto out_error;
	}

	pp_pagetables_pgd = CO_VPTR_PSEUDO_RAM_PAGE_TABLES >> PGDIR_SHIFT;
	pfns	          = cmon->pp_pfns[pp_pagetables_pgd];
	if (pfns == NULL) {
		co_debug_error("CO_VPTR_PSEUDO_RAM_PAGE_TABLES is not mapped, huh?");
		goto out_error;
	}

        // Allocate a page and map it in CO_VPTR_SELF_MAP
	rc = co_monitor_create_ptes(cmon,
			            cmon->import.kernel_swapper_pg_dir,
			            CO_ARCH_PAGE_SIZE,
			            pfns);
	if (!CO_OK(rc)) {
		co_debug_error("error %08x initializing swapper_pg_dir", (int)rc);
		goto out_error;
	}

	// Get PFN of allocated page
	// Create PTE for it on the page directory
	reversed_physical_mapping_offset = (CO_VPTR_PHYSICAL_TO_PSEUDO_PFN_MAP >> PGDIR_SHIFT) *
					   sizeof(linux_pgd_t);

	rc = co_monitor_copy_and_create_pfns(
			cmon,
			cmon->import.kernel_swapper_pg_dir + reversed_physical_mapping_offset,
			sizeof(linux_pgd_t) * cmon->manager->reversed_map_pgds_count,
			(void*)cmon->manager->reversed_map_pgds);

	if (!CO_OK(rc)) {
		co_debug_error("error %08x adding reversed physical mapping", (int)rc);
		goto out_error;
	}

	rc = co_monitor_create_ptes(cmon, CO_VPTR_SELF_MAP, CO_ARCH_PAGE_SIZE, pfns);
	if (!CO_OK(rc)) {
		co_debug_error("error %08x initializing self_map", (int)rc);
		goto out_error;
	}

	rc = co_monitor_get_pfn(cmon, CO_VPTR_SELF_MAP, &self_map_pfn);
	if (!CO_OK(rc)) {
		co_debug_error("error %08x getting self_map pfn", (int)rc);
		goto out_error;
	}

	self_map_page_offset = ((CO_VPTR_SELF_MAP >> PGDIR_SHIFT) * sizeof(linux_pte_t));
	co_debug_ulong(self_map_page_offset);

	rc = co_monitor_create_ptes(
		cmon,
		cmon->import.kernel_swapper_pg_dir + self_map_page_offset,
		sizeof(linux_pte_t),
		&self_map_pfn);

	if (!CO_OK(rc)) {
		co_debug_error("error %08x getting self_map pfn", (int)rc);
		goto out_error;
	}

	passage_page_offset = ((CO_VPTR_PASSAGE_PAGE & ((1 << PGDIR_SHIFT) - 1)) >> CO_ARCH_PAGE_SHIFT) *
			      sizeof(linux_pte_t);

	passage_page_pfn = co_os_virt_to_phys(cmon->passage_page) >> CO_ARCH_PAGE_SHIFT;

	rc = co_monitor_create_ptes(
			cmon,
			CO_VPTR_SELF_MAP + passage_page_offset,
			sizeof(linux_pte_t),
			&passage_page_pfn);

	if (!CO_OK(rc)) {
		co_debug_error("error %08x mapping passage page into the self map", (int)rc);
		goto out_error;
	}

	rc = co_monitor_create_ptes(
			cmon,
			CO_VPTR_SELF_MAP,
			sizeof(linux_pte_t),
			&self_map_pfn);

	if (!CO_OK(rc)) {
		co_debug_error("error %08x initializing self_map", (int)rc);
		goto out_error;
	}

	{
		long 		io_buffer_page;
		long 		io_buffer_num_pages = CO_VPTR_IO_AREA_SIZE >> CO_ARCH_PAGE_SHIFT;
		long 		io_buffer_offset;
		unsigned long 	io_buffer_host_address = (unsigned long)(cmon->io_buffer);

		io_buffer_offset = ((CO_VPTR_IO_AREA_START & ((1 << PGDIR_SHIFT) - 1)) >>
				    CO_ARCH_PAGE_SHIFT) * sizeof(linux_pte_t);

		for (io_buffer_page=0; io_buffer_page < io_buffer_num_pages; io_buffer_page++) {
			co_pfn_t io_buffer_pfn = co_os_virt_to_phys((void*)io_buffer_host_address) >> CO_ARCH_PAGE_SHIFT;

			rc = co_monitor_create_ptes(cmon, CO_VPTR_SELF_MAP + io_buffer_offset,
						    sizeof(linux_pte_t), &io_buffer_pfn);
			if (!CO_OK(rc)) {
				co_debug_error("error %08x initializing io buffer (%ld)", (int)rc, io_buffer_page);
				goto out_error;
			}

			io_buffer_offset       += sizeof(linux_pte_t);
			io_buffer_host_address += CO_ARCH_PAGE_SIZE;
		}
	}

	co_debug("initialization finished");

out_error:
	return rc;
}

static bool_t device_request(co_monitor_t *cmon, co_device_t device, co_param_t *params)
{
	co_debug_lvl(context_switch, 14, "device: %d", device);
	switch (device) {
	case CO_DEVICE_BLOCK: {
		co_block_request_t *request;
		unsigned int unit = params[0];

		co_debug_lvl(blockdev, 13, "blockdev requested (unit %d)", unit);

		request = (co_block_request_t *)(&params[1]);

		co_monitor_block_request(cmon, unit, request);

		return PTRUE;
	}
	case CO_DEVICE_NETWORK: {
		co_network_request_t *network;

		network = (co_network_request_t *)(params);
		network->result = 0;

		if (network->unit >= CO_MODULE_MAX_CONET) {
			co_debug_lvl(network, 12, "invalid network unit %d", network->unit);
			break;
		}

		co_debug_lvl(network, 13, "network unit %d requested", network->unit);

		switch (network->type) {
		case CO_NETWORK_GET_MAC: {
			co_netdev_desc_t *dev = &cmon->config.net_devs[network->unit];

			co_debug_lvl(network, 10, "CO_NETWORK_GET_MAC requested");

			network->result = dev->enabled;

			if (dev->enabled == PFALSE)
				break;

			co_memcpy(network->mac_address, dev->mac_address, sizeof(network->mac_address));
			break;
		}
		default:
			break;
		}

		return PTRUE;
	}
	case CO_DEVICE_FILESYSTEM: {
		co_monitor_file_system(cmon, params[0], params[1], &params[2]);
		return PTRUE;
	}

	case CO_DEVICE_PCI:
		co_pci_request(cmon, params[0]);
		return PTRUE;

	case CO_DEVICE_SCSI:
		co_scsi_request(cmon, params[0], params[1]);
		return PTRUE;

#ifdef CONFIG_COOPERATIVE_VIDEO
	case CO_DEVICE_VIDEO:
		co_video_request(cmon, params[0], params[1]);
		return PTRUE;
#endif

	default:
		break;
	}

	return PTRUE;
}

static co_rc_t callback_return_messages(co_monitor_t *cmon)
{
	co_rc_t rc;
	unsigned char *io_buffer, *io_buffer_end;
	co_queue_t *queue;

	co_passage_page->params[0] = 0;

	if (!cmon->io_buffer)
		return CO_RC(ERROR);

	if (cmon->io_buffer->messages_waiting)
		return CO_RC(OK);

	io_buffer = cmon->io_buffer->buffer;
	io_buffer_end = io_buffer + CO_VPTR_IO_AREA_SIZE - sizeof(co_io_buffer_t);

	co_os_mutex_acquire(cmon->linux_message_queue_mutex);

	cmon->io_buffer->messages_waiting = 0;

	queue = &cmon->linux_message_queue;
	while (co_queue_size(queue) != 0)
	{
		co_message_queue_item_t *message_item;
		co_message_t *message;
		unsigned long size;
		co_linux_message_t *linux_message;

		rc = co_queue_peek_tail(queue, (void **)&message_item);
		if (!CO_OK(rc))
			return rc;

		message = message_item->message;
		size = message->size + sizeof(*message);

		if (io_buffer + size > io_buffer_end) {
			break;
		}

		rc = co_queue_pop_tail(queue, (void **)&message_item);
		if (!CO_OK(rc))
			return rc;

		if ((unsigned long)message->from >= (unsigned long)CO_MODULES_MAX) {
			co_debug_system("BUG! %s:%d", __FILE__, __LINE__);
			co_queue_free(queue, message_item);
			co_os_free(message);
			break;
		}

		if ((unsigned long)message->to >= (unsigned long)CO_MODULES_MAX){
			co_debug_system("BUG! %s:%d", __FILE__, __LINE__);
			co_queue_free(queue, message_item);
			co_os_free(message);
			break;
		}

		linux_message = (co_linux_message_t *)message->data;
		if ((unsigned long)linux_message->device >= (unsigned long)CO_DEVICES_TOTAL){
			co_debug_system("BUG! %s:%d %d %d", __FILE__, __LINE__,
					message->to, message->from);
			co_queue_free(queue, message_item);
			co_os_free(message);
			break;
		}

		cmon->io_buffer->messages_waiting += 1;
		co_queue_free(queue, message_item);
		co_memcpy(io_buffer, message, size);
		io_buffer += size;
		co_os_free(message);
	}

	co_os_mutex_release(cmon->linux_message_queue_mutex);

	co_passage_page->params[0] = io_buffer - (unsigned char *)&cmon->io_buffer->buffer;

	co_debug_lvl(messages, 12, "sending messages to linux (%ld bytes)", co_passage_page->params[0]);

	return CO_RC(OK);
}

static void callback_return_jiffies(co_monitor_t *cmon)
{
	co_timestamp_t timestamp;
	unsigned long long diff;

	co_os_get_timestamp(&timestamp);

	/* Skip timestamp glitches, see http://support.microsoft.com/kb/274323 */
	if (timestamp.quad > cmon->timestamp.quad) {
		diff = cmon->timestamp_reminder + 100 * (timestamp.quad - cmon->timestamp.quad);  /* 100 = HZ value */
		cmon->timestamp_reminder = co_div64_32(&diff, cmon->timestamp_freq.quad);
		cmon->timestamp		 = timestamp;
	} else {
		diff = 0;
	}

	co_passage_page->params[1] = diff; /* jiffies */
}

static void callback_return(co_monitor_t *cmon)
{
	co_passage_page->operation = CO_OPERATION_MESSAGE_FROM_MONITOR;

	callback_return_messages(cmon);
	callback_return_jiffies(cmon);
}

static bool_t co_terminate(co_monitor_t *cmon)
{
	unsigned int len = co_passage_page->params[3];
	char *str = (char *)&co_passage_page->params[4];

	co_os_timer_deactivate(cmon->timer);

	cmon->termination_reason = co_passage_page->params[0];

	if (cmon->termination_reason == CO_TERMINATE_PANIC) {
		if (len >= sizeof(cmon->bug_info.text)-1)
			len = sizeof(cmon->bug_info.text)-1;

		co_memcpy(cmon->bug_info.text, str, len);
		cmon->bug_info.text[len] = 0;

		co_debug_system("PANIC: %s", cmon->bug_info.text);
	} else
	if (cmon->termination_reason == CO_TERMINATE_BUG) {
		cmon->bug_info.code = co_passage_page->params[1];
		cmon->bug_info.line = co_passage_page->params[2];

		if (len < sizeof(cmon->bug_info.text)-1) {
			co_memcpy(cmon->bug_info.text, str, len);
			cmon->bug_info.text[len] = 0;
		} else {
			/* show the end of filename */
			co_snprintf(cmon->bug_info.text, sizeof(cmon->bug_info.text),
				    "...%s", str + len - sizeof(cmon->bug_info.text) + 4);
		}

		co_debug_system("BUG%ld at %s:%ld", cmon->bug_info.code,
				cmon->bug_info.text, cmon->bug_info.line);
	}

	cmon->state = CO_MONITOR_STATE_TERMINATED;

	return PFALSE;
}

static bool_t co_idle(co_monitor_t *cmon)
{
	co_queue_t* queue;

	co_os_wait_sleep(cmon->idle_wait);

	queue = &cmon->linux_message_queue;
	if (co_queue_size(queue) == 0)
		return PFALSE;
	else
		return PTRUE;
}

static void co_free_pages(co_monitor_t *cmon, vm_ptr_t address, int num_pages)
{
	unsigned long scan_address;
	int j;

	scan_address = address;
	for (j=0; j < num_pages; j++) {
		co_monitor_free_and_unmap_page(cmon, scan_address);
		scan_address += CO_ARCH_PAGE_SIZE;
	}
}

static co_rc_t co_alloc_pages(co_monitor_t *cmon, vm_ptr_t address, int num_pages)
{
	unsigned long scan_address;
	co_rc_t rc = CO_RC(OK);
	int i;

	scan_address = address;
	for (i=0; i < num_pages; i++, scan_address += CO_ARCH_PAGE_SIZE) {
		rc = co_monitor_alloc_and_map_page(cmon, scan_address);
		if (!CO_OK(rc)) {
			scan_address = address;
			for (; i; i--, scan_address += CO_ARCH_PAGE_SIZE)
				co_monitor_free_and_unmap_page(cmon, scan_address);
			break;
		}
	}

	return rc;
}

/* Send the physical pages of a buffer to the guest */
static void co_monitor_getpp(co_monitor_t* cmon,
			     void*	   pp_buffer,
			     void*	   host_buffer,
			     int 	   host_buffer_size)
{
	vm_ptr_t	vaddr;
	co_pfn_t	pfn;
	char*		page;
	unsigned long*  pp;
	unsigned long   pa;
	unsigned long 	t;
	void*		buffer;
	int i,size,len;
	co_rc_t rc;

	vaddr = (vm_ptr_t) pp_buffer;
	buffer = host_buffer;

	size = host_buffer_size >> 10;
	while(size > 0) {
		rc = co_monitor_get_pfn(cmon, vaddr, &pfn);
		if (!CO_OK(rc)) {
			co_debug_error("unable to get pfn for vaddr %08lX", vaddr);
			co_passage_page->params[0] = 1;
			return;
		}

		len = ((vaddr + CO_ARCH_PAGE_SIZE) & CO_ARCH_PAGE_MASK) - vaddr;
		if (len > size) len = size;

		page = co_os_map(cmon->manager, pfn);

		pp = (unsigned long *)(page + (vaddr & ~CO_ARCH_PAGE_MASK));
		co_memset(pp, 0, len);

		t = vaddr;
		for(i=0; i < len; i += 4) {
			pa = co_os_virt_to_phys(buffer);

			*pp++ = pa;
			t += CO_ARCH_PAGE_SIZE;
			buffer += CO_ARCH_PAGE_SIZE;
		}
		co_os_unmap(cmon->manager, page, pfn);

		size -= len;
		vaddr += len;
	}

	co_passage_page->params[0] = 0;
	return;
}

// support kernel mode conet module, filter out conet message, return CO_RC_OK if the message was handled.
static co_rc_t co_monitor_filter_linux_message(co_monitor_t *monitor, co_message_t *message)
{
	if (message->from == CO_MODULE_LINUX &&
	    message->to >= CO_MODULE_CONET0 &&
	    message->to <= CO_MODULE_CONET_END) {
		return co_conet_inject_packet_to_adapter(monitor,
				message->to - CO_MODULE_CONET0,
				(void *)(message+1), message->size);
	} else {
		return CO_RC(ERROR);
	}
}

static void incoming_message(co_monitor_t* cmon, co_message_t* message)
{
	co_manager_open_desc_t opened;
	co_rc_t 	       rc;

	co_os_mutex_acquire(cmon->connected_modules_write_lock);
	opened = cmon->connected_modules[message->to];
	if (opened != NULL)
		rc = co_manager_open_ref(opened);
	else
		rc = CO_RC(ERROR);
	co_os_mutex_release(cmon->connected_modules_write_lock);

	if (CO_OK(rc)) {
		// ligong liu, support kernel mode conet, filter for conet message
		if ( co_monitor_filter_linux_message(cmon, message) != CO_RC_OK )
			co_manager_send(cmon->manager, opened, message);
		co_manager_close(cmon->manager, opened);
	}

	switch (message->to) {
	case CO_MODULE_CONSOLE:
		if (message->from == CO_MODULE_LINUX) {
			/* Redirect console operations to kernel shadow */
			co_console_op(cmon->console,
				      (co_console_message_t*)message->data);
		}
		break;
	default:
		break;
	}
}

/* Copy user message to queue */
co_rc_t co_monitor_message_from_user(co_monitor_t* monitor, co_message_t *message)
{
	co_rc_t rc;

	if (message->to == CO_MODULE_LINUX) {
		co_os_mutex_acquire(monitor->linux_message_queue_mutex);
		rc = co_message_dup_to_queue(message, &monitor->linux_message_queue);
		co_os_mutex_release(monitor->linux_message_queue_mutex);
		co_os_wait_wakeup(monitor->idle_wait);
	} else {
		rc = CO_RC(ERROR);
	}

	return rc;
}

co_rc_t co_monitor_message_from_user_free(co_monitor_t *monitor, co_message_t *message)
{
	co_rc_t rc;

	if (message->to == CO_MODULE_LINUX) {
		co_os_mutex_acquire(monitor->linux_message_queue_mutex);
		rc = co_message_mov_to_queue(message, &monitor->linux_message_queue);
		co_os_mutex_release(monitor->linux_message_queue_mutex);
		co_os_wait_wakeup(monitor->idle_wait);
	} else {
		rc = CO_RC(ERROR);
	}

	if (!CO_OK(rc))
		co_os_free(message);

	return rc;
}

/*
 * iteration - returning PTRUE means that the driver will return
 * immediately to Linux instead of returning to the host's
 * userspace and only then to Linux.
 */

static bool_t iteration(co_monitor_t *cmon)
{
	switch (co_passage_page->operation) {
	case CO_OPERATION_FORWARD_INTERRUPT:
	case CO_OPERATION_IDLE:
		callback_return(cmon);
		break;
	}

	co_debug_lvl(context_switch, 14, "switching to linux (%ld)", co_passage_page->operation);
	co_host_switch_wrapper(cmon);

	if (co_passage_page->operation == CO_OPERATION_FORWARD_INTERRUPT)
		co_monitor_arch_real_hardware_interrupt(cmon);
	else
		co_monitor_arch_enable_interrupts();

	switch (co_passage_page->operation) {
	case CO_OPERATION_FREE_PAGES: {
		co_free_pages(cmon, co_passage_page->params[0], co_passage_page->params[1]);
		return PTRUE;
	}

	case CO_OPERATION_ALLOC_PAGES: {
		co_rc_t rc;
		rc = co_alloc_pages(cmon, co_passage_page->params[0], co_passage_page->params[1]);
		co_passage_page->params[4] = (unsigned long)(rc);
		return PTRUE;
	}

	case CO_OPERATION_FORWARD_INTERRUPT: {
		co_debug_lvl(context_switch, 15, "switching from linux (CO_OPERATION_FORWARD_INTERRUPT), %d",
			     cmon->timer_interrupt);

		if (cmon->timer_interrupt) {
			cmon->timer_interrupt = PFALSE;
			/* Return to userspace once in a while */
			return PFALSE;
		}

		return PTRUE;
	}
	case CO_OPERATION_TERMINATE:
		return co_terminate(cmon);

	case CO_OPERATION_IDLE:
		return co_idle(cmon);

	case CO_OPERATION_GETPP:
		co_monitor_getpp(cmon, (void *)co_passage_page->params[0], (void *)co_passage_page->params[1],
			co_passage_page->params[2]);
		return PTRUE;

	case CO_OPERATION_MESSAGE_TO_MONITOR: {
		co_message_t *message;

		co_debug_lvl(context_switch, 14, "switching from linux (CO_OPERATION_MESSAGE_TO_MONITOR)");

		message = (co_message_t *)cmon->io_buffer->buffer;
		if (message  &&  message->to < CO_MONITOR_MODULES_COUNT)
			/* Redirect operation to user level */
			incoming_message(cmon, message);

		cmon->io_buffer->messages_waiting = 0;

		return PTRUE;
	}

	case CO_OPERATION_DEVICE: {
		unsigned long device = co_passage_page->params[0];
		co_debug_lvl(context_switch, 14, "switching from linux (CO_OPERATION_DEVICE)");
		return device_request(cmon, device, &co_passage_page->params[1]);
	}
	case CO_OPERATION_GET_TIME: {
		co_debug_lvl(context_switch, 14, "switching from linux (CO_OPERATION_GET_TIME)");
		co_passage_page->params[0] = co_os_get_time();
		return PTRUE;
	}
	case CO_OPERATION_GET_HIGH_PREC_TIME: {
		co_os_get_timestamp_freq((co_timestamp_t *)&co_passage_page->params[0],
					 (co_timestamp_t *)&co_passage_page->params[2]);
		return PTRUE;
	}

        case CO_OPERATION_DEBUG_LINE:
        case CO_OPERATION_TRACE_POINT:
                return PTRUE;

	default:
		co_debug_lvl(context_switch, 5, "unknown operation %ld not handled", co_passage_page->operation);
		return PFALSE;
	}

	return PTRUE;
}

static void free_file_blockdevice(co_monitor_t *cmon, co_block_dev_t *dev)
{
	co_monitor_file_block_dev_t *fdev = (co_monitor_file_block_dev_t *)dev;

	co_monitor_file_block_shutdown(fdev);
	co_monitor_free(cmon, dev);
}

static co_rc_t load_configuration(co_monitor_t *cmon)
{
	co_rc_t rc = CO_RC_OK;
	unsigned int i;

	for (i=0; i < CO_MODULE_MAX_COSCSI; i++) {
		co_scsi_dev_t *dev;
		co_scsi_dev_desc_t *cp = &cmon->config.scsi_devs[i];
		if (!cp->enabled) continue;

		rc = co_monitor_malloc(cmon, sizeof(co_scsi_dev_t), (void **)&dev);
		if (!CO_OK(rc)) goto error_0;

		co_memset(dev, 0, sizeof(*dev));
		dev->conf = cp;

		cmon->scsi_devs[i] = dev;
	}

	for (i=0; i < CO_MODULE_MAX_COBD; i++) {
		co_monitor_file_block_dev_t *dev;
		co_block_dev_desc_t *conf_dev = &cmon->config.block_devs[i];
		if (!conf_dev->enabled)
			continue;

		rc = co_monitor_malloc(cmon, sizeof(co_monitor_file_block_dev_t), (void **)&dev);
		if (!CO_OK(rc))
			goto error_1;

		rc = co_monitor_file_block_init(cmon, dev, &conf_dev->pathname);
		if (CO_OK(rc)) {
			dev->dev.conf = conf_dev;
			co_debug("cobd%d: enabled (%p)", i, dev);
			co_monitor_block_register_device(cmon, i, (co_block_dev_t *)dev);
			dev->dev.free = free_file_blockdevice;
		} else {
			co_monitor_free(cmon, dev);
			co_debug_error("cobd%d: cannot enable (%08x)", i, (int)rc);
			goto error_1;
		}
	}

	for (i = 0; i < CO_MODULE_MAX_COFS; i++) {
		co_cofsdev_desc_t *desc = &cmon->config.cofs_devs[i];
		if (!desc->enabled)
			continue;

		rc = co_monitor_file_system_init(cmon, i, desc);
		if (!CO_OK(rc))
			goto error_2;

	}

#ifdef CONFIG_COOPERATIVE_VIDEO
	for(i=0; i < CO_MODULE_MAX_COVIDEO; i++) {
		co_video_dev_desc_t *cp = &cmon->config.video_devs[i];
		co_video_dev_t *dev;

		if (!cp->enabled) continue;

		rc = co_monitor_malloc(cmon, sizeof(co_video_dev_t), (void **)&dev);
		if (!CO_OK(rc)) goto error_3;
		cmon->video_devs[i] = dev;

		rc = co_monitor_video_device_init(cmon, i, cp);
		if (!CO_OK(rc)) goto error_3;

        }
#endif

	rc = co_pci_setconfig(cmon);

	return rc;

#ifdef CONFIG_COOPERATIVE_VIDEO
error_3:
	co_monitor_unregister_video_devices(cmon);
#endif
error_2:
	co_monitor_unregister_filesystems(cmon);
error_1:
	co_monitor_unregister_and_free_block_devices(cmon);
error_0:
	co_monitor_unregister_and_free_scsi_devices(cmon);

	return rc;
}

static void timer_callback(void *data)
{
        co_monitor_t *cmon = (co_monitor_t *)data;

        cmon->timer_interrupt = PTRUE;

	co_os_wait_wakeup(cmon->idle_wait);
}

static void free_pseudo_physical_memory(co_monitor_t *monitor)
{
	int i, j;

	if (!monitor->pp_pfns)
		return;

	co_debug("freeing page frames for pseudo physical RAM");

	for (i=0; i < PTRS_PER_PGD; i++) {
		if (!monitor->pp_pfns[i])
			continue;

		for (j=0; j < PTRS_PER_PTE; j++)
			if (monitor->pp_pfns[i][j] != 0)
				co_os_put_page(monitor->manager, monitor->pp_pfns[i][j]);

		co_monitor_free(monitor, monitor->pp_pfns[i]);
	}

	co_monitor_free(monitor, monitor->pp_pfns);
	monitor->pp_pfns = NULL;

	co_debug("done freeing");
}

static co_rc_t alloc_pp_ram_mapping(co_monitor_t *monitor)
{
	co_rc_t rc;
	unsigned long full_page_tables_size;
	unsigned long partial_page_table_size;

	co_debug("allocating page frames for pseudo physical RAM");

	rc = co_monitor_malloc(monitor, sizeof(co_pfn_t *)*PTRS_PER_PGD, (void **)&monitor->pp_pfns);
	if (!CO_OK(rc))
		return rc;

	co_memset(monitor->pp_pfns, 0, sizeof(co_pfn_t *)*PTRS_PER_PGD);

	full_page_tables_size = CO_ARCH_PAGE_SIZE * (monitor->memory_size >> CO_ARCH_PMD_SHIFT);
	partial_page_table_size = sizeof(unsigned long) *
		((monitor->memory_size & ~CO_ARCH_PMD_MASK) >> (CO_ARCH_PAGE_SHIFT));

	rc = co_monitor_scan_and_create_pfns(
		monitor,
		CO_VPTR_PSEUDO_RAM_PAGE_TABLES,
		full_page_tables_size);

	if (CO_OK(rc)) {
		if (partial_page_table_size) {
			rc = co_monitor_scan_and_create_pfns(
				monitor,
				CO_VPTR_PSEUDO_RAM_PAGE_TABLES + full_page_tables_size,
				partial_page_table_size);
		}
	}

	if (!CO_OK(rc)) {
		free_pseudo_physical_memory(monitor);
	}

	return rc;
}

static co_rc_t alloc_shared_page(co_monitor_t *cmon)
{
	co_rc_t rc = CO_RC_OK;

	cmon->shared = co_os_alloc_pages(1);
	if (!cmon->shared)
		return CO_RC(ERROR);

	rc = co_os_userspace_map(cmon->shared, 1, &cmon->shared_user_address, &cmon->shared_handle);
	if (!CO_OK(rc)) {
		co_os_free_pages(cmon->shared, 1);
		return rc;
	}

	return rc;
}

static void free_shared_page(co_monitor_t *cmon)
{
	co_os_userspace_unmap(cmon->shared_user_address, cmon->shared_handle, 1);
	co_os_free_pages(cmon->shared, 1);
}

static co_rc_t load_section(co_monitor_t *cmon, co_monitor_ioctl_load_section_t *params)
{
	co_rc_t rc = CO_RC(OK);

	if (cmon->state != CO_MONITOR_STATE_INITIALIZED)
		return CO_RC(ERROR);

	if (params->user_ptr) {
		co_debug("loading section at 0x%lx (0x%lx bytes)", params->address, params->size);
		rc = co_monitor_copy_region(cmon, params->address, params->size, params->buf);
	} else {
		rc = co_monitor_copy_region(cmon, params->address, params->size, NULL);
	}

	return rc;
}

static co_rc_t load_initrd(co_monitor_t *cmon, co_monitor_ioctl_load_initrd_t *params)
{
	co_rc_t rc = CO_RC(OK);
	unsigned long address, pages;

	if (cmon->state != CO_MONITOR_STATE_INITIALIZED)
		return CO_RC(ERROR);

	pages = ((params->size + CO_ARCH_PAGE_SIZE) >> CO_ARCH_PAGE_SHIFT);

        /*
	 * Put initrd at the end of the address space.
	 */
	address = CO_ARCH_KERNEL_OFFSET + cmon->memory_size - (pages << CO_ARCH_PAGE_SHIFT);

	co_debug("initrd address: %lx (0x%lx pages)", address, pages);

	if (address <= cmon->core_end + 0x100000) {
		co_debug_error("initrd is too close to the kernel code, not enough memory)");

		/* We are too close to the kernel code, not enough memory */
		return CO_RC(ERROR);
	}

	rc = co_monitor_copy_region(cmon, address, params->size, params->buf);
	if (!CO_OK(rc)) {
		co_debug_error("initrd copy failed (%x)", (int)rc);
		return rc;
	}

	cmon->initrd_address = address;
	cmon->initrd_size = params->size;

	return rc;
}


static co_rc_t start(co_monitor_t *cmon)
{
	co_rc_t rc;
	co_boot_params_t *params;

	if (cmon->state != CO_MONITOR_STATE_INITIALIZED) {
		co_debug_error("invalid state");
		return CO_RC(ERROR);
	}

	rc = guest_address_space_init(cmon);
	if (!CO_OK(rc)) {
		co_debug_error("error %08x initializing coLinux context", (int)rc);
		return rc;
	}

	co_os_get_timestamp_freq(&cmon->timestamp, &cmon->timestamp_freq);

	co_os_timer_activate(cmon->timer);

	co_passage_page->operation = CO_OPERATION_START;

	params = (co_boot_params_t *)co_passage_page->params;

	params->co_core_end	= cmon->core_end;
	params->co_memory_size	= cmon->memory_size;
	params->co_initrd 	= (void *)cmon->initrd_address;
	params->co_initrd_size	= cmon->initrd_size;
	params->co_cpu_khz	= co_os_get_cpu_khz();

	co_memcpy(&params->co_boot_parameters,
		cmon->config.boot_parameters_line,
		sizeof(cmon->config.boot_parameters_line));

	cmon->state = CO_MONITOR_STATE_STARTED;

	return CO_RC(OK);
}

static co_rc_t run(co_monitor_t *cmon,
		   co_monitor_ioctl_run_t *params,
		   unsigned long out_size,
		   unsigned long *return_size)
{
	*return_size = sizeof(*params);

	if (cmon->state == CO_MONITOR_STATE_RUNNING) {
		bool_t ret;
		do {
			ret = iteration(cmon);
		} while (ret);
		return CO_RC(OK);
	} else if (cmon->state == CO_MONITOR_STATE_STARTED) {
		cmon->state = CO_MONITOR_STATE_RUNNING;
		iteration(cmon);
		return CO_RC(OK);
	}

	if (cmon->state == CO_MONITOR_STATE_TERMINATED)
		return CO_RC(INSTANCE_TERMINATED);

	return CO_RC(ERROR);
}


co_rc_t co_monitor_create(co_manager_t*		     manager,
			  co_manager_ioctl_create_t* params,
			  co_monitor_t**	     cmon_out)
{
	co_symbols_import_t* import = &params->import;
	co_monitor_t*	     cmon;
	co_rc_t		     rc     = CO_RC_OK;

	if (params->config.magic_size != sizeof(co_config_t))
		return CO_RC(VERSION_MISMATCHED);

	cmon = co_os_malloc(sizeof(*cmon));
	if (!cmon)
		return CO_RC(OUT_OF_MEMORY);

	co_memset(cmon, 0, sizeof(*cmon));

	cmon->manager = manager;
	cmon->state   = CO_MONITOR_STATE_EMPTY;
	cmon->id      = co_os_current_id();

	rc = co_console_create(&params->config.console, &cmon->console);
	if (!CO_OK(rc))
		goto out_free_monitor;

	rc = co_os_mutex_create(&cmon->connected_modules_write_lock);
	if (!CO_OK(rc))
		goto out_free_console;

	rc = co_os_mutex_create(&cmon->linux_message_queue_mutex);
	if (!CO_OK(rc))
		goto out_free_mutex1;

	rc = co_os_wait_create(&cmon->idle_wait);
	if (!CO_OK(rc))
		goto out_free_mutex2;

	params->id = cmon->id;

	cmon->io_buffer = co_os_malloc(CO_VPTR_IO_AREA_SIZE);
	if (cmon->io_buffer == NULL) {
		rc = CO_RC(OUT_OF_MEMORY);
		goto out_free_wait;
	}
	co_memset(cmon->io_buffer, 0, CO_VPTR_IO_AREA_SIZE);

	rc = alloc_shared_page(cmon);
	if (!CO_OK(rc))
		goto out_free_buffer;

	params->shared_user_address = cmon->shared_user_address;
	rc = co_queue_init(&cmon->linux_message_queue);
	if (!CO_OK(rc))
		goto out_free_shared_page;

	rc = co_monitor_os_init(cmon);
	if (!CO_OK(rc))
		goto out_free_linux_message_queue;

	cmon->core_vaddr = import->kernel_start;
	cmon->core_pages = (import->kernel_end - import->kernel_start +
	                    CO_ARCH_PAGE_SIZE - 1) >> CO_ARCH_PAGE_SHIFT;
	cmon->core_end	 = cmon->core_vaddr + (cmon->core_pages << CO_ARCH_PAGE_SHIFT);
	cmon->import	 = params->import;
	cmon->config	 = params->config;
	cmon->info	 = params->info;
	cmon->arch_info	 = params->arch_info;

	if (cmon->config.ram_size == 0) {
		/* Use default RAM sizes */

		/*
		 * FIXME: Be careful with that. We don't want to exhaust the host OS's memory
		 * pools because it can destablized it.
		 *
		 * We need to find the minimum requirement for size of the coLinux RAM
		 * for achieving the best performance assuming that the host OS is
		 * caching away the coLinux swap device into the host system's other
		 * RAM.
		 */

		if (cmon->manager->hostmem_amount >= 128) {
			/* Use quarter */
			cmon->memory_size = cmon->manager->hostmem_amount / 4;
		} else {
			cmon->memory_size = 16;
		}
	} else {
		cmon->memory_size = cmon->config.ram_size;
	}

	co_debug("configured to %ld MB", cmon->memory_size);

	if (cmon->memory_size < 8)
		cmon->memory_size = 8;
	else if (cmon->memory_size > CO_LOWMEMORY_MAX_MB)
		cmon->memory_size = CO_LOWMEMORY_MAX_MB;

	co_debug("after adjustments: %ld MB", cmon->memory_size);

	cmon->memory_size <<= 20; /* Megify */

	if (cmon->manager->hostmem_used + cmon->memory_size > cmon->manager->hostmem_usage_limit) {
		rc			    = CO_RC(HOSTMEM_USE_LIMIT_REACHED);
		params->actual_memsize_used = cmon->memory_size;
		goto out_free_os_dep;
	}

	cmon->manager->hostmem_used += cmon->memory_size;

	cmon->physical_frames	 = cmon->memory_size >> CO_ARCH_PAGE_SHIFT;
	cmon->end_physical	 = CO_ARCH_KERNEL_OFFSET + cmon->memory_size;
	cmon->passage_page_vaddr = CO_VPTR_PASSAGE_PAGE;

	rc = alloc_pp_ram_mapping(cmon);
        if (!CO_OK(rc)) {
		goto out_revert_used_mem;
	}

        rc = co_os_timer_create(&timer_callback, cmon, 10, &cmon->timer); /* HZ value */
        if (!CO_OK(rc)) {
                co_debug_error("error %08x creating host OS timer", (int)rc);
                goto out_free_pp;
        }

	rc = load_configuration(cmon);
	if (!CO_OK(rc)) {
		co_debug_error("error %08x loading monitor configuration", (int)rc);
		goto out_destroy_timer;
	}

	co_os_mutex_acquire(manager->lock);
	cmon->refcount = 1;
	manager->monitors_count++;
	cmon->listed_in_manager = PTRUE;
	co_list_add_head(&cmon->node, &manager->monitors);
	co_os_mutex_release(manager->lock);

	cmon->state = CO_MONITOR_STATE_INITIALIZED;

	*cmon_out = cmon;

	return CO_RC(OK);

/* error path */
out_destroy_timer:
        co_os_timer_destroy(cmon->timer);

out_free_pp:
	free_pseudo_physical_memory(cmon);

out_revert_used_mem:
	cmon->manager->hostmem_used -= cmon->memory_size;

out_free_os_dep:
	co_monitor_os_exit(cmon);

out_free_linux_message_queue:
	co_queue_flush(&cmon->linux_message_queue);

out_free_shared_page:
	free_shared_page(cmon);

out_free_buffer:
	co_os_free(cmon->io_buffer);

out_free_wait:
	co_os_wait_destroy(cmon->idle_wait);

out_free_mutex2:
	co_os_mutex_destroy(cmon->connected_modules_write_lock);

out_free_mutex1:
	co_os_mutex_destroy(cmon->linux_message_queue_mutex);

out_free_console:
	co_console_destroy(cmon->console);

out_free_monitor:
	co_os_free(cmon);

	return rc;
}


static co_rc_t co_monitor_destroy(co_monitor_t *cmon, bool_t user_context)
{
	co_manager_t* manager;

	manager = cmon->manager;

	co_debug("cleaning up");
	co_debug("before free: %ld blocks", cmon->blocks_allocated);

	if (cmon->state == CO_MONITOR_STATE_RUNNING ||
	    cmon->state == CO_MONITOR_STATE_STARTED)
	{
                co_os_timer_deactivate(cmon->timer);
	}

	/* Can't unmap from kernel context, see Bug #2587396 */
	if (!user_context)
		cmon->shared_user_address = NULL;

	co_monitor_unregister_and_free_scsi_devices(cmon);
	co_monitor_unregister_and_free_block_devices(cmon);
	co_monitor_unregister_filesystems(cmon);
#ifdef CONFIG_COOPERATIVE_VIDEO
	co_monitor_unregister_video_devices(cmon);
#endif
	free_pseudo_physical_memory(cmon);
	manager->hostmem_used -= cmon->memory_size;
	co_os_free(cmon->io_buffer);
	free_shared_page(cmon);
	co_monitor_os_exit(cmon);
	co_queue_flush(&cmon->linux_message_queue);
        co_os_timer_destroy(cmon->timer);
	co_os_mutex_destroy(cmon->connected_modules_write_lock);
	co_os_mutex_destroy(cmon->linux_message_queue_mutex);
	co_console_destroy(cmon->console);
	co_monitor_arch_passage_page_free(cmon);

	co_debug("after free: %ld blocks", cmon->blocks_allocated);
	co_os_free(cmon);

	return CO_RC_OK;
}

static void send_monitor_end_messages(co_monitor_t *cmon)
{
	co_manager_open_desc_t opened;
	int i;

	co_os_mutex_acquire(cmon->connected_modules_write_lock);
	for (i = 0; i < CO_MONITOR_MODULES_COUNT; i++) {
		opened = cmon->connected_modules[i];
		if (!opened)
			continue;

		co_manager_send_eof(cmon->manager, opened);
		cmon->connected_modules[i] = NULL;
		co_manager_close(cmon->manager, opened);
	}
	co_os_mutex_release(cmon->connected_modules_write_lock);
}

/*
 * Decrement monitor reference count.
 *
 * Each daemon opens a connection to the driver. The reference count is
 * incremented/decremented on every open/close.
 * Only after the count reaches zero, the monitor object is destroyed.
 */
co_rc_t co_monitor_refdown(co_monitor_t* cmon, bool_t user_context, bool_t monitor_owner)
{
	co_manager_t* manager;

	if (!(cmon->refcount > 0))
		return CO_RC(ERROR);

	manager = cmon->manager;

	co_os_mutex_acquire(manager->lock);
	if (monitor_owner  &&  cmon->listed_in_manager) {
		cmon->listed_in_manager = PFALSE;
		manager->monitors_count--;
		co_list_del(&cmon->node);
	}
	co_os_mutex_release(manager->lock);

	if (monitor_owner)
		send_monitor_end_messages(cmon);

	if (--cmon->refcount == 0)
		return co_monitor_destroy(cmon, monitor_owner);

	return CO_RC(OK);
}

static co_rc_t co_monitor_user_reset(co_monitor_t *monitor)
{
	co_rc_t rc;

	monitor->state = CO_MONITOR_STATE_EMPTY;

	co_os_mutex_acquire(monitor->linux_message_queue_mutex);
	co_queue_flush(&monitor->linux_message_queue);
	co_os_mutex_release(monitor->linux_message_queue_mutex);

	free_pseudo_physical_memory(monitor);
	rc = alloc_pp_ram_mapping(monitor);
	if (!CO_OK(rc))
		goto out;

	co_monitor_unregister_and_free_scsi_devices(monitor);
	co_monitor_unregister_and_free_block_devices(monitor);
	co_monitor_unregister_filesystems(monitor);
#ifdef CONFIG_COOPERATIVE_VIDEO
	co_monitor_unregister_video_devices(monitor);
#endif

	rc = load_configuration(monitor);
	if (!CO_OK(rc)) {
		free_pseudo_physical_memory(monitor);
		goto out;
	}

	co_os_mutex_acquire(monitor->manager->lock);
	if (!monitor->listed_in_manager) {
		monitor->listed_in_manager = PTRUE;
		co_list_add_head(&monitor->node, &monitor->manager->monitors);
	}
	co_os_mutex_release(monitor->manager->lock);

	co_memset(monitor->io_buffer, 0, CO_VPTR_IO_AREA_SIZE);

	monitor->state = CO_MONITOR_STATE_INITIALIZED;
	monitor->termination_reason = CO_TERMINATE_END;

out:
	return rc;
}

static co_rc_t co_monitor_user_get_state(co_monitor_t* monitor, co_monitor_ioctl_get_state_t* params)
{
	params->monitor_state	   = monitor->state;
	params->termination_reason = monitor->termination_reason;
	params->bug_info	   = monitor->bug_info;

	return CO_RC(OK);
}

static co_rc_t co_monitor_user_get_console(co_monitor_t*                   monitor,
                                           co_monitor_ioctl_get_console_t* params)
{
	co_message_t*		co_message;
	co_console_message_t*	message;
	co_console_cell_t*	cellp;
	unsigned long		size;
	int 			y;
	int			config_x = monitor->console->config.x;
	int			config_y = monitor->console->config.y;
	int			max_y = monitor->console->config.max_y;
	int			bytes_per_line = config_x * sizeof(co_console_cell_t);

	params->config = monitor->console->config;

	size = co_offsetof(co_console_message_t, putcs.data) + bytes_per_line;
	co_message = co_os_malloc(size + sizeof(*co_message));
	if (!co_message)
		return CO_RC(OUT_OF_MEMORY);

	message = (co_console_message_t*)co_message->data;

	// send the scrollback buffer via init command
	co_message->from     = CO_MODULE_MONITOR;
	co_message->to       = CO_MODULE_CONSOLE;
	co_message->priority = CO_PRIORITY_DISCARDABLE;
	co_message->type     = CO_MESSAGE_TYPE_STRING;
	co_message->size     = size;

	// send the scroll buffer via a special CO operation
	message->type        = CO_OPERATION_CONSOLE_INIT_SCROLLBUFFER;
	message->putcs.x     = 0;
	message->putcs.count = config_x;

	cellp = monitor->console->buffer + config_y * config_x;
	for (y = config_y; y < max_y; y++, cellp += config_x)
	{
		co_memcpy(&message->putcs.data, cellp, bytes_per_line);
		message->putcs.y = y;

		/* Redirect each string operation to user level */
		incoming_message(monitor, co_message);
	}

	// send the viewable area via putcs command
	message->type = CO_OPERATION_CONSOLE_PUTCS;

	cellp = monitor->console->screen;
	for (y = 0; y < config_y; y++, cellp += config_x)
	{
		co_memcpy(&message->putcs.data, cellp, bytes_per_line);
		message->putcs.y = y;

		/* Redirect each string operation to user level */
		incoming_message(monitor, co_message);
	}

	co_message->size = (char*)(&message->cursor + 1) - (char*)message;

	message->type   = CO_OPERATION_CONSOLE_CURSOR_MOVE;
	message->cursor = monitor->console->cursor;

	/* Redirect cursor operation to user level */
	incoming_message(monitor, co_message);

	co_os_free(co_message);

	return CO_RC(OK);
}

co_rc_t co_monitor_ioctl(co_monitor_t* 		     cmon,
			 co_manager_ioctl_monitor_t* io_buffer,
			 unsigned long 		     in_size,
			 unsigned long 		     out_size,
			 unsigned long*		     return_size,
			 co_manager_open_desc_t      opened_manager)
{
	co_rc_t rc = CO_RC_ERROR;

	switch (io_buffer->op) {
	case CO_MONITOR_IOCTL_CLOSE: {
		co_monitor_refdown(cmon, PTRUE, opened_manager->monitor_owner);
		opened_manager->monitor       = NULL;
		opened_manager->monitor_owner = PFALSE;
		return CO_RC_OK;
	}
	case CO_MONITOR_IOCTL_GET_CONSOLE: {
		co_monitor_ioctl_get_console_t* params;

		*return_size = sizeof(*params);
		params       = (typeof(params))(io_buffer);

		return co_monitor_user_get_console(cmon, params);
	}
#ifdef CONFIG_COOPERATIVE_VIDEO
	case CO_MONITOR_IOCTL_VIDEO_ATTACH: {
		co_monitor_ioctl_video_t* params;

		*return_size = sizeof(*params);
		params       = (typeof(params))(io_buffer);

		return co_video_attach(cmon, params);
	}
	case CO_MONITOR_IOCTL_VIDEO_DETACH: {
		co_monitor_ioctl_video_t* params;

		*return_size = sizeof(*params);
		params       = (typeof(params))(io_buffer);

		return co_video_detach(cmon, params);
	}
#endif
	case CO_MONITOR_IOCTL_CONET_BIND_ADAPTER: {
		co_monitor_ioctl_conet_bind_adapter_t *params;

		params = (typeof(params))(io_buffer);
		if(params->conet_proto == CO_CONET_BRIDGE)
			return co_conet_bind_adapter(cmon,
						     params->conet_unit,
						     params->netcfg_id,
						     params->promisc_mode,
						     params->mac_address);
		else
			return rc;
	}
	case CO_MONITOR_IOCTL_CONET_UNBIND_ADAPTER: {
		co_monitor_ioctl_conet_unbind_adapter_t *params;

		params = (typeof(params))(io_buffer);

		return co_conet_unbind_adapter(cmon, params->conet_unit);
	}
	default:
		break;
	}

	if (!opened_manager->monitor_owner)
		return rc;

	switch (io_buffer->op) {
	case CO_MONITOR_IOCTL_GET_STATE: {
		co_monitor_ioctl_get_state_t *params;

		*return_size = sizeof(*params);
		params       = (typeof(params))(io_buffer);

		return co_monitor_user_get_state(cmon, params);
	}
	case CO_MONITOR_IOCTL_RESET: {
		return co_monitor_user_reset(cmon);
	}
	case CO_MONITOR_IOCTL_LOAD_SECTION: {
		return load_section(cmon, (co_monitor_ioctl_load_section_t*)io_buffer);
	}
	case CO_MONITOR_IOCTL_LOAD_INITRD: {
		return load_initrd(cmon, (co_monitor_ioctl_load_initrd_t*)io_buffer);
	}
	case CO_MONITOR_IOCTL_START: {
		return start(cmon);
	}
	case CO_MONITOR_IOCTL_RUN: {
		return run(cmon, (co_monitor_ioctl_run_t*)io_buffer, out_size, return_size);
	}
	default:
		return rc;
	}

	return rc;
}
