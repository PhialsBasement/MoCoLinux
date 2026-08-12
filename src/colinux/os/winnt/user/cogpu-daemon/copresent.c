/*
 * Host half of the R2 CoPresent metadata service.
 *
 * The guest announces two pinned pages over a loopback-only control socket.
 * BIND/PRESENT/UNBIND records then move through those pages; finished pixels
 * remain virglrenderer textures in this process. The renderer and presenter
 * are deliberately called only from cogpu's main thread because
 * virglrenderer is not a cross-thread API.
 */

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../../../user/copresent/copresent.h"
#include "copresent.h"
#include "present.h"
#include "vrend.h"

void *resolve_gpa(void *ctx, uint64_t gpa, uint32_t len);
/* The pointers below outlive the accept that resolved them, so the pages are
 * held with the pinned resolve against the daemon's slice eviction. */
void *cogpu_resolve_gpa_pinned(uint64_t gpa, uint32_t len, void **token);
void cogpu_unpin(void *token);
void logline(const char *fmt, ...);
void cogpu_ring_doorbell(void);

struct copresent_channel {
	SOCKET ctl;
	unsigned char *page[COPRESENT_REGION_PAGES];
	void *pin[COPRESENT_REGION_PAGES];
	struct copresent_region *header;
	uint32_t generation;
};

static CRITICAL_SECTION channel_lock;
static int channel_lock_ready;
static struct copresent_channel *channel;
static SOCKET listen_socket = INVALID_SOCKET;
static uint32_t active_generation;

static int recv_all(SOCKET socket, void *buffer, int bytes)
{
	char *p = (char *)buffer;

	while (bytes > 0) {
		int got = recv(socket, p, bytes, 0);

		if (got <= 0)
			return -1;
		p += got;
		bytes -= got;
	}
	return 0;
}

static void *region_at(struct copresent_channel *c, size_t offset,
			       size_t bytes)
{
	size_t page = offset / COPRESENT_PAGE_BYTES;
	size_t within = offset % COPRESENT_PAGE_BYTES;

	if (!c || page >= COPRESENT_REGION_PAGES ||
	    bytes > COPRESENT_PAGE_BYTES - within)
		return NULL;
	return c->page[page] + within;
}

static void channel_free(struct copresent_channel *c)
{
	unsigned int i;

	if (!c)
		return;
	if (c->header)
		c->header->closed = 1;
	MemoryBarrier();
	if (c->ctl != INVALID_SOCKET)
		closesocket(c->ctl);
	for (i = 0; i < COPRESENT_REGION_PAGES; i++)
		if (c->pin[i])
			cogpu_unpin(c->pin[i]);
	free(c);
}

static struct copresent_channel *accept_channel(SOCKET ctl)
{
	struct copresent_channel *c = NULL;
	struct copresent_open open_record;
	uint64_t gpa[COPRESENT_REGION_PAGES];
	u_long nonblocking = 1;
	char ack = 0;
	unsigned int i;

	if (recv_all(ctl, &open_record, sizeof(open_record)) != 0)
		return NULL;
	if (open_record.magic != COPRESENT_MAGIC ||
	    open_record.version != COPRESENT_VERSION ||
	    open_record.generation == 0 ||
	    open_record.npages != COPRESENT_REGION_PAGES) {
		logline("CoPresent: refused open magic 0x%08x version %u"
			" generation %u pages %u\n", open_record.magic,
			open_record.version, open_record.generation,
			open_record.npages);
		return NULL;
	}
	if (recv_all(ctl, gpa, sizeof(gpa)) != 0)
		return NULL;

	c = (struct copresent_channel *)calloc(1, sizeof(*c));
	if (!c)
		return NULL;
	c->ctl = ctl;
	c->generation = open_record.generation;
	for (i = 0; i < COPRESENT_REGION_PAGES; i++) {
		c->page[i] = (unsigned char *)cogpu_resolve_gpa_pinned(gpa[i],
			COPRESENT_PAGE_BYTES, &c->pin[i]);
		if (!c->page[i]) {
			logline("CoPresent: page %u at 0x%llx is outside guest RAM\n",
				i, (unsigned long long)gpa[i]);
			goto fail;
		}
	}
	c->header = (struct copresent_region *)c->page[0];
	if (c->header->magic != COPRESENT_MAGIC ||
	    c->header->version != COPRESENT_VERSION ||
	    c->header->generation != c->generation ||
	    c->header->closed) {
		logline("CoPresent: shared header does not match its open record\n");
		goto fail;
	}
	ioctlsocket(ctl, FIONBIO, &nonblocking);
	ack = 1;
	if (send(ctl, &ack, 1, 0) != 1)
		goto fail;
	return c;

fail:
	/* The caller owns ctl until a successful return. */
	c->ctl = INVALID_SOCKET;
	channel_free(c);
	return NULL;
}

static DWORD WINAPI listen_thread(LPVOID unused)
{
	(void)unused;
	for (;;) {
		SOCKET ctl = accept(listen_socket, NULL, NULL);
		struct copresent_channel *next;
		struct copresent_channel *old;

		if (ctl == INVALID_SOCKET)
			break;
		next = accept_channel(ctl);
		if (!next) {
			closesocket(ctl);
			continue;
		}

		EnterCriticalSection(&channel_lock);
		old = channel;
		channel = next;
		LeaveCriticalSection(&channel_lock);
		channel_free(old);
		logline("CoPresent: guest broker generation %u mapped (%u bytes)\n",
			next->generation, COPRESENT_REGION_BYTES);
	}
	return 0;
}

static int channel_is_closed(struct copresent_channel *c)
{
	char byte;
	int got;

	if (!c || c->header->closed)
		return 1;
	got = recv(c->ctl, &byte, 1, MSG_PEEK);
	if (got == 0)
		return 1;
	if (got == SOCKET_ERROR && WSAGetLastError() != WSAEWOULDBLOCK)
		return 1;
	return 0;
}

static int write_response(struct copresent_channel *c,
			  const struct copresent_record *record)
{
	struct copresent_region *h = c->header;
	uint32_t head;
	uint32_t tail;
	struct copresent_record *slot;
	unsigned int tries;

	for (tries = 0; tries < 100000; tries++) {
		head = h->rx_head;
		MemoryBarrier();
		tail = h->rx_tail;
		if ((uint32_t)(head - tail) < COPRESENT_RING_SLOTS)
			break;
		YieldProcessor();
	}
	if (tries == 100000) {
		logline("CoPresent: release ring stayed full; dropping generation %u\n",
			c->generation);
		return -1;
	}
	slot = (struct copresent_record *)region_at(c,
		__builtin_offsetof(struct copresent_region, rx) +
		(size_t)(head % COPRESENT_RING_SLOTS) * sizeof(*slot),
		sizeof(*slot));
	if (!slot)
		return -1;
	*slot = *record;
	MemoryBarrier();
	h->rx_head = head + 1;
	h->host_heartbeat++;
	cogpu_ring_doorbell();
	return 0;
}

static int handle_record(struct copresent_channel *c,
			 const struct copresent_record *request)
{
	struct copresent_record response = *request;
	struct cogpu_vrend_resource_info info;
	int status = COPRESENT_BAD_RECORD;

	if (request->version != COPRESENT_VERSION ||
	    request->generation != c->generation ||
	    request->connection == 0) {
		response.opcode = COPRESENT_ERROR;
		response.status = COPRESENT_BAD_RECORD;
		return write_response(c, &response);
	}

	switch (request->opcode) {
	case COPRESENT_BIND:
		response.opcode = COPRESENT_BOUND;
		if (cogpu_vrend_resource_info(request->resource_id, &info) != 0)
			status = COPRESENT_NO_RESOURCE;
		else
			status = cogpu_present_r2_bind(request, &info);
		break;
	case COPRESENT_PRESENT:
		response.opcode = COPRESENT_RELEASE;
		status = cogpu_present_r2_present(request);
		break;
	case COPRESENT_UNBIND:
		response.opcode = COPRESENT_UNBOUND;
		status = cogpu_present_r2_unbind(request);
		break;
	default:
		response.opcode = COPRESENT_ERROR;
		status = COPRESENT_BAD_RECORD;
		break;
	}
	response.status = (uint32_t)status;
	return write_response(c, &response);
}

int cogpu_copresent_poll(void)
{
	struct copresent_channel *dead = NULL;
	struct copresent_channel *c;
	unsigned int handled = 0;
	int rc = 0;

	if (!channel_lock_ready)
		return 0;
	EnterCriticalSection(&channel_lock);
	c = channel;
	if (!c) {
		if (active_generation) {
			cogpu_present_r2_reset(active_generation);
			active_generation = 0;
		}
		LeaveCriticalSection(&channel_lock);
		return 0;
	}
	if (channel_is_closed(c)) {
		channel = NULL;
		dead = c;
		if (active_generation)
			cogpu_present_r2_reset(active_generation);
		active_generation = 0;
		LeaveCriticalSection(&channel_lock);
		channel_free(dead);
		logline("CoPresent: guest broker disconnected\n");
		return 0;
	}
	if (active_generation != c->generation) {
		if (active_generation)
			cogpu_present_r2_reset(active_generation);
		active_generation = c->generation;
		cogpu_present_r2_reset(active_generation);
	}

	while (handled < 64) {
		struct copresent_region *h = c->header;
		uint32_t head = h->tx_head;
		uint32_t tail;
		struct copresent_record *slot;
		struct copresent_record record;

		MemoryBarrier();
		tail = h->tx_tail;
		if (head == tail)
			break;
		if ((uint32_t)(head - tail) > COPRESENT_RING_SLOTS) {
			logline("CoPresent: corrupt producer occupancy %u\n",
				(uint32_t)(head - tail));
			rc = -1;
			break;
		}
		slot = (struct copresent_record *)region_at(c,
			__builtin_offsetof(struct copresent_region, tx) +
			(size_t)(tail % COPRESENT_RING_SLOTS) * sizeof(*slot),
			sizeof(*slot));
		if (!slot) {
			rc = -1;
			break;
		}
		record = *slot;
		MemoryBarrier();
		h->tx_tail = tail + 1;
		if (handle_record(c, &record) != 0) {
			rc = -1;
			break;
		}
		handled++;
	}
	c->header->host_heartbeat++;
	LeaveCriticalSection(&channel_lock);
	return rc;
}

int cogpu_copresent_start(void)
{
	struct sockaddr_in address;
	HANDLE thread;
	WSADATA wsa;
	int one = 1;

	if (channel_lock_ready)
		return 0;
	if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
		return -1;
	InitializeCriticalSection(&channel_lock);
	channel_lock_ready = 1;

	listen_socket = socket(AF_INET, SOCK_STREAM, 0);
	if (listen_socket == INVALID_SOCKET)
		goto fail;
	setsockopt(listen_socket, SOL_SOCKET, SO_REUSEADDR,
		   (const char *)&one, sizeof(one));
	memset(&address, 0, sizeof(address));
	address.sin_family = AF_INET;
	address.sin_port = htons(COPRESENT_CONTROL_PORT);
	address.sin_addr.s_addr = inet_addr("127.0.0.1");
	if (bind(listen_socket, (struct sockaddr *)&address, sizeof(address)) != 0 ||
	    listen(listen_socket, 2) != 0)
		goto fail;
	thread = CreateThread(NULL, 0, listen_thread, NULL, 0, NULL);
	if (!thread)
		goto fail;
	CloseHandle(thread);
	logline("CoPresent: listening on %d for metadata-only R2 channels\n",
		COPRESENT_CONTROL_PORT);
	return 0;

fail:
	logline("CoPresent: listener startup failed (%d)\n", WSAGetLastError());
	cogpu_copresent_fini();
	return -1;
}

void cogpu_copresent_fini(void)
{
	struct copresent_channel *old = NULL;

	if (listen_socket != INVALID_SOCKET) {
		closesocket(listen_socket);
		listen_socket = INVALID_SOCKET;
	}
	if (channel_lock_ready) {
		EnterCriticalSection(&channel_lock);
		old = channel;
		channel = NULL;
		LeaveCriticalSection(&channel_lock);
		channel_free(old);
		if (active_generation)
			cogpu_present_r2_reset(active_generation);
		active_generation = 0;
		/* The listener exits after closesocket wakes accept(). Keep the
		 * critical section storage valid until process teardown so a listener
		 * that was between accept and publication cannot race its deletion. */
	}
}
