/*
 * This source code is a part of coLinux source package.
 *
 * Ligong Liu <liulg@mail.reachstone.com>, 2008 (c)
 *
 * The code is licensed under the GPL. See the COPYING file at
 * the root directory.
 *
 */

#define NDIS50

#include "ddk.h"
#include <ntdddisk.h>
#include <ddk/ndis.h>
#include <ddk/xfilter.h>

#include <colinux/os/alloc.h>
#include <colinux/common/libc.h>
#include <colinux/common/unicode.h>
#include <colinux/kernel/monitor.h>
#include <colinux/os/kernel/time.h>
#include <colinux/os/winnt/monitor.h>
#include <colinux/os/winnt/kernel/conet.h>

// #define CONET_DEBUG

#ifdef CONET_DEBUG
# define conet_debug(fmt, args...) co_debug_lvl(network, 10, fmt, ## args )
#else
# define conet_debug(fmt, args...)
#endif
#define conet_err_debug(fmt, args...) co_debug_lvl(network, 3, fmt, ## args )

// Forward reference
static void DDKAPI co_conet_proto_transfer_complete(
	IN NDIS_HANDLE		ProtocolBindingContext,
	IN PNDIS_PACKET		Packet,
	IN NDIS_STATUS		Status,
	IN UINT			BytesTransferred);

/*
 * NdisInitializeEvent/WaitEvent/SetEvent/ResetEvent used to be reimplemented
 * here because the w32api NDIS headers of the time did not declare them.
 * mingw-w64 declares all four and libndis exports them, so use the real ones.
 *
 * Both NdisWaitEvent call sites below pass MsToWait = 0, which the real
 * function treats as an indefinite wait, matching what the local version did.
 */

static void co_FreeBuffersAndPacket(
	IN PNDIS_PACKET		Packet
	)
{
	PNDIS_BUFFER TransferBuffer;

	while (1) {
		NdisUnchainBufferAtFront(Packet, &TransferBuffer);
		if (!TransferBuffer)
			break;

		conet_debug("NdisFreeBuffer %p packet %p", TransferBuffer, Packet);
		NdisFreeBuffer(TransferBuffer);
	}

	conet_debug("NdisFreePacket(Packet = %p)", Packet);
	NdisFreePacket(Packet);
}

static VOID DDKAPI co_conet_transfer_message_routine(PDEVICE_OBJECT DeviceObject, PVOID Context)
{
	conet_message_transfer_context_t *context;
	PIO_WORKITEM	work_item;
	co_monitor_t	*monitor;
	co_message_t	*message;
	co_rc_t		rc;

	context = (conet_message_transfer_context_t *)Context;
	monitor = context->monitor;
	message = context->message;
	work_item = context->work_item;

	conet_debug("enter: context = %p, monitor = %p, message = %p, work item = %p",
		context, monitor, message, work_item);

	rc = co_monitor_message_from_user_free(monitor, message);
	if ( !CO_OK(rc) )
		conet_debug("allocate queue item fail, ignore message %p", message);

	conet_debug("free context %p, free work item %p", context, work_item);
	co_os_free(context);
	IoFreeWorkItem(work_item);

	conet_debug("leave");
}

static void co_conet_transfer_message(conet_adapter_t *adapter, co_message_t *message)
{
	conet_message_transfer_context_t *context;
	extern PDEVICE_OBJECT coLinux_DeviceObject;

	conet_debug("enter: adapter = %p, message = %p", adapter, message);

	context = (conet_message_transfer_context_t *)co_os_malloc(sizeof(conet_message_transfer_context_t));
	if ( context ) {
		context->work_item = IoAllocateWorkItem(coLinux_DeviceObject);
		if ( !context->work_item ) {
			co_os_free(context);
			conet_err_debug("leave: allocate io work item for colinux device fail");
			return;
		}

		conet_debug("queue work item to co_conet_transfer_message_routine delay work queue");
		context->monitor = adapter->monitor;
		context->message = message;
		IoQueueWorkItem(context->work_item, co_conet_transfer_message_routine, DelayedWorkQueue, context);
	}

	conet_debug("leave: success");
}

static conet_adapter_t *co_conet_create_adapter(co_monitor_t *monitor, int conet_unit)
{
	conet_adapter_t *adapter;
	NDIS_STATUS	Status;

	conet_debug("enter: monitor = %p, id = %u, conet_unit = %d",
		monitor, monitor->id, conet_unit);

	adapter = (conet_adapter_t*)co_os_malloc(sizeof(conet_adapter_t));
	if ( !adapter ) {
		conet_err_debug("leave: allocate conet adapter fail");
		return NULL;
	}

	co_list_init(&adapter->list_node);
	adapter->monitor = monitor;
	adapter->conet_unit = conet_unit;
	adapter->binding_status = NDIS_STATUS_SUCCESS;
	adapter->binding_handle = NULL;
	NdisInitializeEvent(&adapter->binding_event);

	conet_debug("allocating packet pool ...");
	NdisAllocatePacketPool(&Status, &adapter->packet_pool, CONET_MAX_PACKET_DESCRIPTOR, CONET_MAX_PACKET_SIZE);
	if ( Status != NDIS_STATUS_SUCCESS ) {
		conet_err_debug("leave: allocating packet pool fail, Status %x", Status);
		co_os_free(adapter);
		return NULL;
	}

	conet_debug("allocating buffer pool ...");
	NdisAllocateBufferPool(&Status, &adapter->buffer_pool, CONET_MAX_PACKET_BUFFER);
	if ( Status != NDIS_STATUS_SUCCESS ) {
		conet_err_debug("leave: allocating buffer pool fail, Status %x", Status);
		NdisFreePacketPool(adapter->packet_pool);
		co_os_free(adapter);
		return NULL;
	}

	conet_debug("reset promisc mode and clear mac address");
	adapter->general_status = NDIS_STATUS_MEDIA_CONNECT;
	adapter->promisc = FALSE;
	RtlZeroMemory(adapter->macaddr, sizeof(adapter->macaddr));

	NdisAllocateSpinLock(&adapter->pending_transfer_lock);
	co_list_init(&adapter->pending_transfer_list);
	NdisAllocateSpinLock(&adapter->pending_send_lock);
	co_list_init(&adapter->pending_send_list);

	conet_debug("leave: create conet adapter %p success", adapter);
	return adapter;
}

static void co_conet_free_adapter(conet_adapter_t *adapter)
{
	conet_packet_t	*packet, *next_packet;

	conet_debug("enter: adapter = %p", adapter);

	if ( adapter ) {
		conet_debug("free pending transfer packet ...");
		NdisAcquireSpinLock(&adapter->pending_transfer_lock);
		co_list_each_entry_safe(packet, next_packet, &adapter->pending_transfer_list, list_node) {
			co_list_del(&packet->list_node);
			conet_debug("Unchain and free Buffers (Packet = %p)", packet);
			co_FreeBuffersAndPacket((PNDIS_PACKET)packet);
		}
		NdisReleaseSpinLock(&adapter->pending_transfer_lock);

		conet_debug("free pending send packet ...");
		NdisAcquireSpinLock(&adapter->pending_send_lock);
		co_list_each_entry_safe(packet, next_packet, &adapter->pending_send_list, list_node) {
			co_list_del(&packet->list_node);
			conet_debug("Unchain and free Buffers (Packet = %p)", packet);
			co_FreeBuffersAndPacket((PNDIS_PACKET)packet);
		}
		NdisReleaseSpinLock(&adapter->pending_send_lock);

		NdisFreeSpinLock(&adapter->pending_transfer_lock);
		NdisFreeSpinLock(&adapter->pending_send_lock);

		conet_debug("free packet pool and buffer pool...");
		NdisFreePacketPool(adapter->packet_pool);
		NdisFreeBufferPool(adapter->buffer_pool);

		conet_debug("free adapter %p", adapter);
		co_os_free(adapter);
	}

	conet_debug("leave: adapter = %p", adapter);
}

static void DDKAPI co_conet_proto_bind_adapter(
	OUT PNDIS_STATUS	Status,
	IN  NDIS_HANDLE		BindContext,
	IN  PNDIS_STRING	DeviceName,
	IN  PVOID		SystemSpecific1,
	IN  PVOID		SystemSpecific2
	)
{
	conet_binding_context	*context;
	co_monitor_t		*monitor;
	conet_adapter_t		*adapter;
	co_monitor_osdep_t	*osdep;
	UINT			mediumIndex;
	NDIS_MEDIUM		mediumArray = NdisMedium802_3;
	NDIS_STATUS		OpenErrorStatus;

	context = (conet_binding_context*)BindContext;
	monitor = context->monitor;
	adapter = context->adapter;
	osdep = monitor->osdep;

	conet_debug("enter: monitor = %p, adapter = %p", monitor, adapter);

	conet_debug("enter: NdisOpenAdapter(protocol_handle = %p, DeviceName = %ls)",
		osdep->conet_protocol, DeviceName->Buffer);
	NdisOpenAdapter(Status,
			&OpenErrorStatus,
			&adapter->binding_handle,
			&mediumIndex,
			&mediumArray,
			sizeof(mediumArray)/sizeof(NDIS_MEDIUM),
			osdep->conet_protocol,
			adapter,	// protocol binding context
			DeviceName,
			0,
			NULL);

        if ( *Status == NDIS_STATUS_PENDING ) {
		conet_debug("wait NdisOpenAdapter to complete ...");
		NdisWaitEvent(&adapter->binding_event, 0);
		*Status = adapter->binding_status;
		conet_debug("NdisOpenAdapter completed, Status = %x", *Status);
        }

	conet_debug("leave: Status = %x, binding_handle = %p",
		*Status, adapter->binding_handle);
}

static void DDKAPI co_conet_proto_unbind_adapter(
	OUT PNDIS_STATUS	Status,
	IN  NDIS_HANDLE		ProtocolBindingContext,
	IN  NDIS_HANDLE		UnbindContext
	)
{
	conet_adapter_t		*adapter = (conet_adapter_t*)ProtocolBindingContext;

	conet_debug("enter: adapter = %p", adapter);

	conet_debug("NdisCloseAdapter(binding_handle = %p)",
		adapter->binding_handle);
	NdisResetEvent(&adapter->binding_event);
	NdisCloseAdapter(Status, adapter->binding_handle);

	if ( *Status == NDIS_STATUS_PENDING ) {
		conet_debug("wait NdisCloseAdapter to complete ...");
		NdisWaitEvent(&adapter->binding_event, 0);
		*Status = adapter->binding_status;
		conet_debug("NdisCloseAdapter completed, Status = %x", *Status);
	}

	adapter->binding_handle = NULL;
	conet_debug("leave: Status = %x", *Status);
}

static void DDKAPI co_conet_proto_open_adapter_complete(
	IN NDIS_HANDLE		ProtocolBindingContext,
	IN NDIS_STATUS		Status,
	IN NDIS_STATUS		OpenErrorStatus
	)
{
	conet_adapter_t	*adapter = (conet_adapter_t*)ProtocolBindingContext;

	conet_debug("adapter = %p, Status = %x",
		adapter, Status);
	adapter->binding_status = Status;
	NdisSetEvent(&adapter->binding_event);
}

static void DDKAPI co_conet_proto_close_adapter_complete(
	IN NDIS_HANDLE		ProtocolBindingContext,
	IN NDIS_STATUS		Status
	)
{
	conet_adapter_t	*adapter = (conet_adapter_t*)ProtocolBindingContext;

	conet_debug("adapter = %p, Status = %x",
		adapter, Status);
	adapter->binding_status = Status;
	NdisSetEvent(&adapter->binding_event);
}

static bool_t co_conet_proto_filter_packet(
	conet_adapter_t		*adapter,
	PVOID			HeaderBuffer
	)
{
	PETHER_HDR		pEthHdr = (PETHER_HDR)HeaderBuffer;
	UINT			Result;

	ETH_COMPARE_NETWORK_ADDRESSES_EQ(pEthHdr->h_dest, adapter->macaddr, &Result);
	if ( Result == 0 ) {
		/* ether dst mac */
		conet_debug("MAC %2.2x:%2.2x:%2.2x:%2.2x:%2.2x:%2.2x match",
			pEthHdr->h_dest[0], pEthHdr->h_dest[1], pEthHdr->h_dest[2],
			pEthHdr->h_dest[3], pEthHdr->h_dest[4], pEthHdr->h_dest[5]);
		return TRUE;
	}

	if ( ETH_IS_MULTICAST(pEthHdr->h_dest) ) {
		/* ether multicast (implicates broadcast) */
		conet_debug("MAC %2.2x:%2.2x:%2.2x:%2.2x:%2.2x:%2.2x multicast",
			pEthHdr->h_dest[0], pEthHdr->h_dest[1], pEthHdr->h_dest[2],
			pEthHdr->h_dest[3], pEthHdr->h_dest[4], pEthHdr->h_dest[5]);

		ETH_COMPARE_NETWORK_ADDRESSES_EQ(pEthHdr->h_source, adapter->macaddr, &Result);
		if ( Result == 0 ) {
			/* ether src mac */
			conet_debug("MAC %2.2x:%2.2x:%2.2x:%2.2x:%2.2x:%2.2x echo",
				pEthHdr->h_source[0], pEthHdr->h_source[1], pEthHdr->h_source[2],
				pEthHdr->h_source[3], pEthHdr->h_source[4], pEthHdr->h_source[5]);
			return FALSE;
		}

		return TRUE;
	}

	conet_debug("not matched");
	return FALSE;
}

static NDIS_STATUS DDKAPI co_conet_proto_receive(
	IN NDIS_HANDLE		ProtocolBindingContext,
	IN NDIS_HANDLE		MacReceiveContext,
	IN PVOID		HeaderBuffer,
	IN UINT			HeaderBufferSize,
	IN PVOID		LookAheadBuffer,
	IN UINT			LookAheadBufferSize,
	IN UINT			PacketSize
	)
{
	conet_adapter_t		*adapter = (conet_adapter_t*)ProtocolBindingContext;
	conet_packet_t		*packet;
	PNDIS_BUFFER		pHeaderBuffer, TransferBuffer;
	NDIS_STATUS		Status;

	conet_debug("enter: adapter = %p, Header = %p/%d, Buffer = %p/%d, PacketSize = %d",
		adapter, HeaderBuffer, HeaderBufferSize, LookAheadBuffer, LookAheadBufferSize, PacketSize);

	if ( HeaderBufferSize + PacketSize > CONET_MAX_PACKET_SIZE ) {
		conet_debug("leave: packet is too big, ignore");
		return NDIS_STATUS_SUCCESS; // ignore big packet
	}

	if ( LookAheadBufferSize == PacketSize ) {
		struct conet_message {
			co_message_t message;
			co_linux_message_t linux;
			char data[HeaderBufferSize + LookAheadBufferSize];
		} *message;

		conet_debug("receive a full packet, filter MAC address");

		if ( !co_conet_proto_filter_packet(adapter, HeaderBuffer) ) {
			conet_debug("leave: not our packet");
			return NDIS_STATUS_SUCCESS;
		}

		message = (struct conet_message *)co_os_malloc(sizeof(struct conet_message));
		if ( !message ) {
			conet_debug("leave: allocate message fail");
			return NDIS_STATUS_RESOURCES;
		}

		message->message.from = CO_MODULE_CONET0 + adapter->conet_unit;
		message->message.to = CO_MODULE_LINUX;
		message->message.priority = CO_PRIORITY_DISCARDABLE;
		message->message.type = CO_MESSAGE_TYPE_OTHER;
		message->message.size = sizeof(struct conet_message) - sizeof(message->message);
		message->linux.device = CO_DEVICE_NETWORK;
		message->linux.unit = adapter->conet_unit;
		message->linux.size = HeaderBufferSize + LookAheadBufferSize;
		NdisMoveMemory(message->data, HeaderBuffer, HeaderBufferSize);
		NdisMoveMemory(message->data + HeaderBufferSize, LookAheadBuffer, LookAheadBufferSize);

		conet_debug("from CO_MODULE_CONET%d, to CO_MODULE_LINUX",
			adapter->conet_unit);

		co_conet_transfer_message(adapter, (co_message_t *)message);

		conet_debug("leave: co_conet_transfer_message complete success");
		return NDIS_STATUS_SUCCESS;
	}

	conet_debug("allocate packet ...");
	NdisAllocatePacket(&Status, (PNDIS_PACKET*)(&packet), adapter->packet_pool);
	if ( Status != NDIS_STATUS_SUCCESS ) {
		conet_err_debug("leave: allocate packet fail, Status = %x", Status);
		return Status;
	}

	conet_debug("allocate packet buffer...");
	NdisAllocateBuffer(&Status,
			  &pHeaderBuffer,
			  adapter->buffer_pool,
			  (PVOID)packet->packet_buffer,
			  HeaderBufferSize + LookAheadBufferSize);

	if ( Status != NDIS_STATUS_SUCCESS ) {
		conet_err_debug("leave: allocate packet buffer fail, size = %d, Status = %x",
			HeaderBufferSize + LookAheadBufferSize, Status);
		NdisFreePacket((PNDIS_PACKET)packet);
		return NDIS_STATUS_SUCCESS;
	}

	conet_debug("move header and look ahead buffer to packet buffer");
	NdisChainBufferAtFront((PNDIS_PACKET)packet, pHeaderBuffer);
	NdisMoveMemory(packet->packet_buffer, HeaderBuffer, HeaderBufferSize);
	NdisMoveMemory(&packet->packet_buffer[HeaderBufferSize], LookAheadBuffer, LookAheadBufferSize);

	conet_debug("allocate another packet buffer...");
	NdisAllocateBuffer(&Status,
			&TransferBuffer,
			adapter->packet_pool,
			(PVOID)&packet->packet_buffer[HeaderBufferSize + LookAheadBufferSize],
			PacketSize - LookAheadBufferSize);

	if ( Status != NDIS_STATUS_SUCCESS ) {
		conet_err_debug("leave: allocate packet buffer fails, size = %d",
			PacketSize - LookAheadBufferSize);
		conet_debug("Unchain and free Buffers (Packet = %p)", packet);
		co_FreeBuffersAndPacket((PNDIS_PACKET)packet);
		return NDIS_STATUS_SUCCESS;
	}

	NdisChainBufferAtFront((PNDIS_PACKET)packet, TransferBuffer);

	co_list_init(&packet->list_node);
	packet->bytes_transferred = 0;
	packet->transfer_status = NDIS_STATUS_SUCCESS;

	NdisAcquireSpinLock(&adapter->pending_transfer_lock);
	co_list_add_head(&packet->list_node, &adapter->pending_transfer_list);
	NdisReleaseSpinLock(&adapter->pending_transfer_lock);

	conet_debug("start NdisTransferData ...");
	NdisTransferData(&packet->transfer_status,
			adapter->binding_handle,
			MacReceiveContext,
			LookAheadBufferSize,
			PacketSize - LookAheadBufferSize,
			(PNDIS_PACKET)packet,
			&packet->bytes_transferred);

	if ( packet->transfer_status != NDIS_STATUS_PENDING ) {
		conet_debug("NdisTransferData complete, start process");
		co_conet_proto_transfer_complete(ProtocolBindingContext,
						 (PNDIS_PACKET)packet,
						  packet->transfer_status,
						  packet->bytes_transferred);
	}

	conet_debug("leave: success");
	return NDIS_STATUS_SUCCESS;
}

static void DDKAPI co_conet_proto_receive_complete(
	IN NDIS_HANDLE		ProtocolBindingContext
	)
{
#ifdef CONET_DEBUG
	conet_adapter_t		*adapter = (conet_adapter_t*)ProtocolBindingContext;
#endif

	conet_debug("adapter = %p", adapter);
}

static void DDKAPI co_conet_proto_request_complete(
	IN NDIS_HANDLE		ProtocolBindingContext,
	IN PNDIS_REQUEST	Request,
	IN NDIS_STATUS		Status
	)
{
#ifdef CONET_DEBUG
	conet_adapter_t		*adapter = (conet_adapter_t*)ProtocolBindingContext;
#endif

	conet_debug("adapter = %p, NdisRequest = %p", adapter, Request);
	co_os_free(Request);
}

static void DDKAPI co_conet_proto_send_complete(
	IN NDIS_HANDLE		ProtocolBindingContext,
	IN PNDIS_PACKET		Packet,
	IN NDIS_STATUS		Status
	)
{
	conet_adapter_t		*adapter = (conet_adapter_t*)ProtocolBindingContext;
	conet_packet_t		*packet = (conet_packet_t*)Packet;

	conet_debug("adapter = %p, Packet = %p", adapter, Packet);
	NdisAcquireSpinLock(&adapter->pending_send_lock);
	co_list_del(&packet->list_node);
	NdisReleaseSpinLock(&adapter->pending_send_lock);

	conet_debug("Unchain and free Buffers (Packet = %p)", Packet);
	co_FreeBuffersAndPacket(Packet);
}

static void DDKAPI co_conet_proto_reset_complete(
	IN NDIS_HANDLE		ProtocolBindingContext,
	IN NDIS_STATUS		Status
	)
{
#ifdef CONET_DEBUG
	conet_adapter_t		*adapter = (conet_adapter_t*)ProtocolBindingContext;
#endif
	conet_debug("adapter = %p", adapter);
}

static void DDKAPI co_conet_proto_status(
	IN NDIS_HANDLE		ProtocolBindingContext,
	IN NDIS_STATUS		Status,
	IN PVOID		StatusBuffer,
	IN UINT			StatusBufferSize
	)
{
	conet_adapter_t		*adapter = (conet_adapter_t*)ProtocolBindingContext;

	conet_debug("adapter = %p, Status = %x", adapter, Status);
	adapter->general_status = Status;
}

static void DDKAPI co_conet_proto_status_complete(
	IN NDIS_HANDLE		ProtocolBindingContext
	)
{
	conet_adapter_t		*adapter = (conet_adapter_t*)ProtocolBindingContext;

	struct conet_message {
		co_message_t message;
		co_linux_message_t linux;
		int connected;
	} *message;

	conet_debug("enter: adapter = %p", adapter);

	if ( adapter->general_status != NDIS_STATUS_MEDIA_CONNECT &&
	     adapter->general_status != NDIS_STATUS_MEDIA_DISCONNECT )
	{
		conet_debug("leave: not media status, ignore");
		return;
	}

	message = (struct conet_message *)co_os_malloc(sizeof(struct conet_message));
	if ( !message ) {
		conet_err_debug("leave: allocate message fail");
		return;
	}

	message->message.from = CO_MODULE_CONET0 + adapter->conet_unit;
	message->message.to = CO_MODULE_LINUX;
	message->message.priority = CO_PRIORITY_DISCARDABLE;
	message->message.type = CO_MESSAGE_TYPE_STRING; // special message type!
	message->message.size = sizeof(struct conet_message) - sizeof (message->message);
	message->linux.device = CO_DEVICE_NETWORK;
	message->linux.unit = adapter->conet_unit;
	message->linux.size = sizeof(int);
	message->connected = (adapter->general_status == NDIS_STATUS_MEDIA_CONNECT) ? TRUE : FALSE;

	conet_debug("from CO_MODULE_CONET%d, to CO_MODULE_LINUX",
		adapter->conet_unit);

	co_conet_transfer_message(adapter, (co_message_t *)message);

	conet_debug("leave: adapter = %p, notify message to colinux", adapter);
}

static void DDKAPI co_conet_proto_transfer_complete(
	IN NDIS_HANDLE		ProtocolBindingContext,
	IN PNDIS_PACKET		Packet,
	IN NDIS_STATUS		Status,
	IN UINT			BytesTransferred
	)
{
	conet_adapter_t		*adapter = (conet_adapter_t*)ProtocolBindingContext;
	conet_packet_t		*packet = (conet_packet_t *)Packet;
	PNDIS_BUFFER		TransferBuffer;
	UINT			TotalPacketLength;

	conet_debug("enter: adapter = %p, Packet = %p, Status = %x, BytesTransferred = %d",
		adapter, Packet, Status, BytesTransferred);

	NdisAcquireSpinLock(&adapter->pending_transfer_lock);
	co_list_del(&packet->list_node);
	NdisReleaseSpinLock(&adapter->pending_transfer_lock);

	conet_debug("unchain buffer from front and chain to back");
	NdisUnchainBufferAtFront(Packet, &TransferBuffer);
	NdisChainBufferAtBack(Packet, TransferBuffer);

	NdisQueryPacketLength(Packet, &TotalPacketLength);
	conet_debug("query packet length, TotalPacketLength = %d",
		TotalPacketLength);

	if ( !co_conet_proto_filter_packet(adapter, packet->packet_buffer) ) {
		conet_debug("not our packet");
	} else {
		struct conet_message {
			co_message_t message;
			co_linux_message_t linux;
			char data[TotalPacketLength];
		} *message;

		message = (struct conet_message *)co_os_malloc(sizeof(struct conet_message));
		if ( !message ) {
			conet_err_debug("allocate message fail");
		} else {
			message->message.from = CO_MODULE_CONET0 + adapter->conet_unit;
			message->message.to = CO_MODULE_LINUX;
			message->message.priority = CO_PRIORITY_DISCARDABLE;
			message->message.type = CO_MESSAGE_TYPE_OTHER;
			message->message.size = sizeof(struct conet_message) - sizeof(message->message);
			message->linux.device = CO_DEVICE_NETWORK;
			message->linux.unit = adapter->conet_unit;
			message->linux.size = TotalPacketLength;
			NdisMoveMemory(message->data, packet->packet_buffer, TotalPacketLength);

			conet_debug("from CO_MODULE_CONET%d, to CO_MODULE_LINUX",
				adapter->conet_unit);

			co_conet_transfer_message(adapter, (co_message_t *)message);

			conet_debug("co_conet_queue_message complete");
		}
	}

	conet_debug("Unchain and free Buffers (Packet = %p)", Packet);
	co_FreeBuffersAndPacket(Packet);

	conet_debug("leave");
}

static int DDKAPI co_conet_proto_receive_packet(
	IN NDIS_HANDLE		ProtocolBindingContext,
	IN PNDIS_PACKET		Packet
	)
{
	conet_adapter_t		*adapter = (conet_adapter_t*)ProtocolBindingContext;
	UINT			TotalPacketLength;
	PNDIS_BUFFER		CurrentBuffer;
	PUCHAR			VirtualAddress;
	UINT			CurrentLength;
	int			i;

	conet_debug("enter: adapter = %p, Packet = %p", adapter, Packet);

	NdisQueryPacket(Packet,
			(PUINT)NULL,		// Physical Buffer Count
			(PUINT)NULL,		// Buffer Count
			&CurrentBuffer,		// First Buffer
			&TotalPacketLength	// TotalPacketLength
			);

	conet_debug("first buffer %p, TotalPacketLength %d",
		CurrentBuffer, TotalPacketLength);

	if ( TotalPacketLength > 0 ) {

		struct conet_message {
			co_message_t message;
			co_linux_message_t linux;
			char data[TotalPacketLength];
		} *message;

		message = (struct conet_message *)co_os_malloc(sizeof(struct conet_message));
		if ( !message ) {
			conet_err_debug("leave: allocate message fail, return 0");
			return 0;
		}

		i = 0;
		while (CurrentBuffer) {
			NdisQueryBufferSafe(CurrentBuffer, (PVOID)&VirtualAddress, &CurrentLength,
						NormalPagePriority);
			conet_debug("move packet data from buffer %p, length %d to message buffer",
				VirtualAddress, CurrentLength);
			NdisMoveMemory(&message->data[i], VirtualAddress, CurrentLength);
			i += CurrentLength;
			NdisGetNextBuffer(CurrentBuffer, &CurrentBuffer);
		}

		if ( !co_conet_proto_filter_packet(adapter, message->data) ) {
			conet_debug("not our packet");
			co_os_free(message);
		} else {
			message->message.from = CO_MODULE_CONET0 + adapter->conet_unit;
			message->message.to = CO_MODULE_LINUX;
			message->message.priority = CO_PRIORITY_DISCARDABLE;
			message->message.type = CO_MESSAGE_TYPE_OTHER;
			message->message.size = sizeof(struct conet_message) - sizeof(message->message);
			message->linux.device = CO_DEVICE_NETWORK;
			message->linux.unit = adapter->conet_unit;
			message->linux.size = TotalPacketLength;

			conet_debug("from CO_MODULE_CONET%d, to CO_MODULE_LINUX",
				adapter->conet_unit);

			co_conet_transfer_message(adapter, (co_message_t *)message);
			conet_debug("co_conet_transfer_message complete");
		}
	}

	conet_debug("leave: return 0");
	return 0;
}

static NDIS_STATUS DDKAPI co_conet_proto_pnp_handler(
	IN NDIS_HANDLE		ProtocolBindingContext,
	IN PNET_PNP_EVENT	pNetPnPEvent
	)
{
	NDIS_STATUS		Status  = NDIS_STATUS_SUCCESS;
#ifdef CONET_DEBUG
	conet_adapter_t		*adapter = (conet_adapter_t*)ProtocolBindingContext;
	NET_DEVICE_POWER_STATE	powerState;

	conet_debug("enter: adapter = %p, PnPEvent = %p", adapter, pNetPnPEvent);

	switch(pNetPnPEvent->NetEvent)
	{
	case  NetEventSetPower :
		powerState = *(PNET_DEVICE_POWER_STATE)pNetPnPEvent->Buffer;
		switch (powerState)
		{
		case NetDeviceStateD0:
			conet_debug("NetEventSetPower NetDeviceStataD0, success");
			Status = NDIS_STATUS_SUCCESS;
			break;
		case NetDeviceStateD1:
			conet_debug("NetEventSetPower NetDeviceStataD1, success");
			Status = NDIS_STATUS_SUCCESS;
			break;
		case NetDeviceStateD2:
			conet_debug("NetEventSetPower NetDeviceStataD2, success");
			Status = NDIS_STATUS_SUCCESS;
			break;
		case NetDeviceStateD3:
			conet_debug("NetEventSetPower NetDeviceStataD3, success");
			Status = NDIS_STATUS_SUCCESS;
			break;
		default:
			conet_debug("NetEventSetPower unknown power state %d, not supported",
				powerState);
			Status = NDIS_STATUS_NOT_SUPPORTED;
			break;
		}
		break;
	case NetEventQueryPower:
		conet_debug("NetEventQueryPower, success");
		Status  = NDIS_STATUS_SUCCESS;
		break;
	case NetEventQueryRemoveDevice:
		conet_debug("NetEventQueryRemoveDevice, success");
		Status  = NDIS_STATUS_SUCCESS;
		break;
	case NetEventCancelRemoveDevice:
		conet_debug("NetEventCancelRemoveDevice, success");
		Status  = NDIS_STATUS_SUCCESS;
		break;
	case NetEventReconfigure:
		conet_debug("NetEventReconfigure, success");
		Status  = NDIS_STATUS_SUCCESS;
		break;
	case NetEventBindsComplete:
		conet_debug("NetEventBindsComplete, success");
		Status  = NDIS_STATUS_SUCCESS;
		break;

	case NetEventPnPCapabilities:
		conet_debug("NetEventPnpCapabilities, not supported");
		Status = NDIS_STATUS_NOT_SUPPORTED;
		break;
	case NetEventBindList:
		conet_debug("NetEventBindList, not supported");
		Status = NDIS_STATUS_NOT_SUPPORTED;
		break;
	default:
		conet_debug("unknown event %d, not supported", pNetPnPEvent->NetEvent);
		Status = NDIS_STATUS_NOT_SUPPORTED;
		break;
	}

	conet_debug("leave: adapter = %p", adapter);
#endif /* CONET_DEBUG */
	return Status;
}

co_rc_t co_conet_register_protocol(co_monitor_t *monitor)
{
	co_monitor_osdep_t		*osdep = monitor->osdep;
	NDIS_PROTOCOL_CHARACTERISTICS   protoChar;
	ANSI_STRING			AnsiProtoName;
	NDIS_STRING                     protoName;
	NDIS_STATUS			Status;
	NDIS_HANDLE			protoHandle;

	conet_debug("enter: monitor = %p, id = %u", monitor, monitor->id);

	if ( osdep->conet_protocol ) {
		conet_debug("leave: protocol %s already registered",
			osdep->protocol_name);
		return CO_RC_OK;
	}

	NdisZeroMemory(&protoChar, sizeof(NDIS_PROTOCOL_CHARACTERISTICS));

	// register per monitor instance conet protocol identified by monitor id
	co_snprintf(osdep->protocol_name, sizeof(osdep->protocol_name), "Conet-Bridge-%u", (unsigned)monitor->id);
	RtlInitAnsiString(&AnsiProtoName, osdep->protocol_name);
	Status = RtlAnsiStringToUnicodeString(&protoName, &AnsiProtoName, TRUE);
	if ( Status != STATUS_SUCCESS ) {
		conet_err_debug("leave: convert protocol name %s to unicode fail, Status = %x",
			osdep->protocol_name, Status);
		return CO_RC(ERROR);
	}

	protoChar.MajorNdisVersion	      = 0x05;
	protoChar.MinorNdisVersion	      = 0x00;
	protoChar.Name                        = protoName;
	protoChar.OpenAdapterCompleteHandler  = co_conet_proto_open_adapter_complete;	/* PASSIVE_LEVEL */
	protoChar.CloseAdapterCompleteHandler = co_conet_proto_close_adapter_complete;	/* PASSIVE_LEVEL */
	protoChar.SendCompleteHandler         = co_conet_proto_send_complete;		/* DISPATCH_LEVEL */
	protoChar.TransferDataCompleteHandler = co_conet_proto_transfer_complete;	/* DISPATCH_LEVEL */
	protoChar.ResetCompleteHandler        = co_conet_proto_reset_complete;		/* <= DISPATCH_LEVEL */
	protoChar.RequestCompleteHandler      = co_conet_proto_request_complete;	/* DISPATCH_LEVEL */
	protoChar.ReceiveHandler              = co_conet_proto_receive;			/* DISPATCH_LEVEL */
	protoChar.ReceiveCompleteHandler      = co_conet_proto_receive_complete;	/* DISPATCH_LEVEL */
	protoChar.StatusHandler               = co_conet_proto_status;			/* DISPATCH_LEVEL */
	protoChar.StatusCompleteHandler       = co_conet_proto_status_complete;		/* DISPATCH_LEVEL */
	protoChar.BindAdapterHandler          = co_conet_proto_bind_adapter;		/* PASSIVE_LEVEL */
	protoChar.UnbindAdapterHandler        = co_conet_proto_unbind_adapter;		/* PASSIVE_LEVEL */
	protoChar.UnloadHandler               = NULL;
	protoChar.ReceivePacketHandler        = co_conet_proto_receive_packet;		/* DISPATCH_LEVEL */
	protoChar.PnPEventHandler             = co_conet_proto_pnp_handler;

	co_debug("NdisRegisterProtocol %s, NdisVersion %d.%d",
		osdep->protocol_name, protoChar.MajorNdisVersion, protoChar.MinorNdisVersion);
	NdisRegisterProtocol(&Status, &protoHandle, &protoChar,	sizeof(NDIS_PROTOCOL_CHARACTERISTICS));
	RtlFreeUnicodeString(&protoName);

	if ( !NT_SUCCESS(Status) || !protoHandle ) {
		conet_err_debug("leave: register %s protocol fail, Status = %x",
			osdep->protocol_name, Status);
		return CO_RC(ERROR);
	}

	osdep->conet_protocol = protoHandle;
	co_os_mutex_create(&osdep->conet_mutex);
	co_list_init(&osdep->conet_adapters);

	conet_debug("leave: register %s protocol success, protocol handle %p",
		osdep->protocol_name, osdep->conet_protocol);

	return CO_RC_OK;
}

co_rc_t co_conet_unregister_protocol(co_monitor_t *monitor)
{
	co_monitor_osdep_t	*osdep = monitor->osdep;
	conet_adapter_t		*adapter, *adapter_next;
	NDIS_HANDLE		protoHandle;
	NDIS_STATUS		Status;

	conet_debug("enter: monitor = %p, id = %u", monitor, monitor->id);

	if ( osdep->conet_protocol ) {
		conet_debug("unbind adapters from protocol %s", osdep->protocol_name);
		co_os_mutex_acquire(osdep->conet_mutex);
		co_list_each_entry_safe(adapter, adapter_next, &osdep->conet_adapters, list_node) {
			co_conet_proto_unbind_adapter(&Status, adapter, NULL);
			co_list_del(&adapter->list_node);
			co_conet_free_adapter(adapter);
		}
		co_os_mutex_release(osdep->conet_mutex);

		conet_debug("unregister protocol %s, protocol handle %p",
			osdep->protocol_name, osdep->conet_protocol);
		protoHandle = osdep->conet_protocol;
		osdep->conet_protocol = NULL;
		NdisDeregisterProtocol(&Status, protoHandle);

		co_debug("unregister protocol %s completed, Status = %x",
			osdep->protocol_name, Status);
	} else
		conet_debug("conet bridge protocol not registered");

	conet_debug("leave");
	return CO_RC_OK;
}

co_rc_t co_conet_bind_adapter(co_monitor_t *monitor, int conet_unit, char *netcfg_id, int promisc, char macaddr[6])
{
	co_monitor_osdep_t	*osdep = monitor->osdep;
	conet_adapter_t		*adapter;
	NDIS_STATUS		Status;
	ANSI_STRING		AnsiAdapterName;
	UNICODE_STRING		AdapterName;
	conet_binding_context	context;
	PNDIS_REQUEST		Request;
#ifdef CONET_DEBUG
	unsigned char		*mac = (unsigned char*)macaddr;
#endif

	conet_debug("enter: monitor = %p, conet_unit = %d, netcfg_id = %s, macaddr = %02x:%02x:%02x:%02x:%02x:%02x",
		monitor, conet_unit, netcfg_id, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

	if ( !osdep->conet_protocol ) {
		conet_err_debug("conet bridge protocol for monitor %u not registered!", monitor->id);
		return CO_RC(ERROR);
	}

	co_os_mutex_acquire(osdep->conet_mutex);
	co_list_each_entry(adapter, &osdep->conet_adapters, list_node) {
		if ( adapter->conet_unit == conet_unit ) {
			co_os_mutex_release(osdep->conet_mutex);
			conet_debug("leave: adapter already opened, adapter = %p", adapter);
			return CO_RC_OK;
		}
	}
	co_os_mutex_release(osdep->conet_mutex);

	RtlInitAnsiString(&AnsiAdapterName, netcfg_id);
	Status = RtlAnsiStringToUnicodeString(&AdapterName, &AnsiAdapterName, TRUE);
	if ( Status != STATUS_SUCCESS ) {
		conet_err_debug("leave: convert netcfg_id %s to unicode failed", netcfg_id);
		return CO_RC(ERROR);
	}

	conet_debug("create conet_adapter instance ...");
	adapter = co_conet_create_adapter(monitor, conet_unit);
	if ( !adapter )
	{
		conet_err_debug("leave: create conet_adapter instance failed");
		RtlFreeUnicodeString(&AdapterName);
		return CO_RC(ERROR);
        }

	adapter->promisc = promisc;
	ETH_COPY_NETWORK_ADDRESS(adapter->macaddr, macaddr);

	conet_debug("bind adapter %p to %s protocol ...", adapter, osdep->protocol_name);
	context.monitor = monitor;
	context.adapter = adapter;
	co_conet_proto_bind_adapter(&Status, &context, &AdapterName, NULL, NULL);
	RtlFreeUnicodeString(&AdapterName);

        if ( Status != NDIS_STATUS_SUCCESS ) {
		conet_err_debug("leave: bind adapter %p to protocol %s fail, Status %x",
	    		adapter, osdep->protocol_name, Status);
		co_conet_free_adapter(adapter);
		return CO_RC(ERROR);
        }

	if ( adapter->promisc ) {
		co_debug("set ndis packet filter NDIS_PACKET_TYPE_PROMISCUOUS");
		adapter->packet_filter = NDIS_PACKET_TYPE_PROMISCUOUS;
	} else {
		co_debug("set ndis packet filter NDIS_PACKET_TYPE_ALL_LOCAL");
		adapter->packet_filter = NDIS_PACKET_TYPE_DIRECTED |
					 NDIS_PACKET_TYPE_BROADCAST |
					 NDIS_PACKET_TYPE_ALL_LOCAL ;
	}

	Request = co_os_malloc(sizeof(NDIS_REQUEST));
	if ( Request ) {
		Request->RequestType = NdisRequestSetInformation;
		Request->DATA.SET_INFORMATION.Oid = OID_GEN_CURRENT_PACKET_FILTER;
		Request->DATA.SET_INFORMATION.InformationBuffer = (PVOID)&adapter->packet_filter;
		Request->DATA.SET_INFORMATION.InformationBufferLength = sizeof(adapter->packet_filter);

		conet_debug("send ndis request %p NdisRequestSetInformation, Oid OID_GEN_CURRENT_PACKET_FILTER",
			Request);
		NdisRequest(&Status, adapter->binding_handle, Request);
		if ( Status != NDIS_STATUS_PENDING ) {
			conet_debug("ndis request %p completed", Request);
			co_os_free(Request);
		}
	}

	conet_debug("add adapter %p to conet_adapters list", adapter);
	co_os_mutex_acquire(osdep->conet_mutex);
	co_list_add_head(&adapter->list_node, &osdep->conet_adapters);
	co_os_mutex_release(osdep->conet_mutex);

	conet_debug("bind adapter %p to protocol %s success", adapter, osdep->protocol_name);
	return CO_RC_OK;
}

co_rc_t co_conet_unbind_adapter(co_monitor_t *monitor, int conet_unit)
{
	co_monitor_osdep_t	*osdep = monitor->osdep;
	conet_adapter_t		*adapter;
	NDIS_STATUS		Status;

	conet_debug("enter: monitor = %p, conet_unit = %d", monitor, conet_unit);

	if ( !osdep->conet_protocol ) {
		conet_debug("conet bridge protocol for monitor %u not registered!", monitor->id);
		return CO_RC(ERROR);
	}

	co_os_mutex_acquire(osdep->conet_mutex);
	co_list_each_entry(adapter, &osdep->conet_adapters, list_node) {
		if ( adapter->conet_unit == conet_unit ) {
			conet_debug("found adapter %p, remove from conet adapter list", adapter);
			co_list_del(&adapter->list_node);
			co_os_mutex_release(osdep->conet_mutex);

			conet_debug("unbind adapter %p from protocol %s ...",
				adapter, osdep->protocol_name);
			co_conet_proto_unbind_adapter(&Status, adapter, NULL);

			co_conet_free_adapter(adapter);
			conet_debug("leave: adapter unbind and freed");
			return CO_RC_OK;
		}
	}
	co_os_mutex_release(osdep->conet_mutex);

	conet_debug("leave: adapter not found");
	return CO_RC(ERROR);
}

co_rc_t co_conet_inject_packet_to_adapter(co_monitor_t *monitor, int conet_unit, void *packet_data, int length)
{
	co_monitor_osdep_t	*osdep = monitor->osdep;
	conet_adapter_t		*adapter = NULL;
	NDIS_HANDLE		binding_handle = NULL;
	conet_packet_t		*packet;
	PNDIS_BUFFER		pPacketBuffer;
	NDIS_STATUS		Status;

	conet_debug("enter: monitor = %p, conet_unit = %d, packet_data = %p, length = %d",
		monitor, conet_unit, packet_data, length);

	if ( !osdep->conet_protocol ) {
		conet_debug("conet bridge protocol for monitor %u not registered!", monitor->id);
		return CO_RC(ERROR);
	}

	co_list_each_entry(adapter, &osdep->conet_adapters, list_node) {
		if ( adapter->conet_unit == conet_unit ) {
			conet_debug("found adapter %p", adapter);
			binding_handle = adapter->binding_handle;
			break;
		}
	}

	if ( !binding_handle ) {
		conet_debug("leave: adapter not found, discard packet");
		return CO_RC(ERROR);
	}

	conet_debug("allocate packet ...");
	NdisAllocatePacket(&Status, (PNDIS_PACKET*)(&packet), adapter->packet_pool);
	if ( Status != NDIS_STATUS_SUCCESS) {
		conet_err_debug("leave: allocate packet fail, Status = %x", Status);
		return CO_RC(ERROR);
	}

	conet_debug("allocate packet buffer...");
	NdisAllocateBuffer(&Status,
			  &pPacketBuffer,
			  adapter->buffer_pool,
			  (PVOID)packet->packet_buffer,
			  length);

	if ( Status != NDIS_STATUS_SUCCESS ) {
		conet_err_debug("leave: allocate packet buffer fail, size = %d", length);
		NdisFreePacket((PNDIS_PACKET)packet);
		return CO_RC(ERROR);
	}

	conet_debug("copy packet data to packet buffer");
	NdisChainBufferAtFront((PNDIS_PACKET)packet, pPacketBuffer);
	NdisMoveMemory(packet->packet_buffer, packet_data, length);

	conet_debug("add packet %p to pending send list", packet);
	NdisAcquireSpinLock(&adapter->pending_send_lock);
	co_list_add_head(&packet->list_node, &adapter->pending_send_list);
	NdisReleaseSpinLock(&adapter->pending_send_lock);

	conet_debug("NdisSend(binding_handle = %p, packet = %p)",
		binding_handle, packet);
	NdisSend(&Status, binding_handle, (PNDIS_PACKET)packet);
	if ( Status != NDIS_STATUS_PENDING ) {
		conet_debug("NdisSend Packet %p completed", packet);
		co_conet_proto_send_complete(adapter, (PNDIS_PACKET)packet, Status);
	}

	conet_debug("leave: success");
	return CO_RC_OK;
}
