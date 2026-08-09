/*
 * CoPresent R2 guest broker.
 *
 * Unprivileged clients pass dma-buf and native-fence FDs over a SOCK_SEQPACKET
 * Unix socket. The broker imports each dma-buf on renderD128 solely to obtain
 * its virtio resource id, waits acquire fences, and publishes fixed 64-byte
 * records through two pinned guest pages. It never maps or copies pixels.
 *
 * This process runs as root only because Linux hides PFNs in pagemap from
 * unprivileged users. Render-node access and all rendering remain unprivileged.
 */

#define _GNU_SOURCE

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <libdrm/drm.h>
#include <libdrm/virtgpu_drm.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "copresent.h"

#define COPRESENT_HOST_ADDR "10.0.2.2"
#define BROKER_MAX_BUFFERS  16

struct broker;

struct broker_buffer {
	int active;
	int unbind_pending;
	uint32_t id;
	uint32_t gem_handle;
	uint32_t resource_id;
	uint32_t xid;
	uint32_t width;
	uint32_t height;
};

struct broker_client {
	struct broker *broker;
	int fd;
	uint32_t connection;
	pthread_mutex_t send_lock;
	pthread_mutex_t state_lock;
	struct broker_buffer buffers[BROKER_MAX_BUFFERS];
	struct broker_client *next;
};

struct broker {
	int control;
	int listen_fd;
	int drm_fd;
	unsigned char *allocation;
	struct copresent_region *region;
	uint32_t generation;
	uint32_t next_connection;
	pthread_mutex_t tx_lock;
	pthread_mutex_t clients_lock;
	struct broker_client *clients;
};

static inline void store_release(volatile uint32_t *p, uint32_t value)
{
	__atomic_store_n(p, value, __ATOMIC_RELEASE);
}

static inline uint32_t load_acquire(volatile uint32_t *p)
{
	return __atomic_load_n(p, __ATOMIC_ACQUIRE);
}

static int write_all(int fd, const void *buffer, size_t bytes)
{
	const unsigned char *p = buffer;

	while (bytes) {
		ssize_t wrote = write(fd, p, bytes);

		if (wrote < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		if (wrote == 0)
			return -1;
		p += wrote;
		bytes -= (size_t)wrote;
	}
	return 0;
}

static int region_pfns(void *address, size_t bytes, uint64_t *gpa,
			uint32_t count)
{
	long page_size = sysconf(_SC_PAGESIZE);
	int pagemap;
	uint32_t i;

	if (page_size != COPRESENT_PAGE_BYTES ||
	    bytes != (size_t)count * (size_t)page_size)
		return -1;
	if (mlock(address, bytes) != 0) {
		perror("moco-present: mlock");
		return -1;
	}
	pagemap = open("/proc/self/pagemap", O_RDONLY);
	if (pagemap < 0) {
		perror("moco-present: pagemap");
		return -1;
	}
	for (i = 0; i < count; i++) {
		uint64_t entry;
		off_t offset = (off_t)(((uintptr_t)address / page_size) + i) * 8;

		if (pread(pagemap, &entry, sizeof(entry), offset) != sizeof(entry) ||
		    !(entry & (1ULL << 63))) {
			fprintf(stderr, "moco-present: page %u is not resident\n", i);
			close(pagemap);
			return -1;
		}
		gpa[i] = (entry & ((1ULL << 55) - 1)) * (uint64_t)page_size;
		if (gpa[i] == 0) {
			fprintf(stderr, "moco-present: pagemap hid page %u;"
				" broker must run as root\n", i);
			close(pagemap);
			return -1;
		}
	}
	close(pagemap);
	return 0;
}

static uint32_t make_generation(void)
{
	struct timespec time;
	uint32_t generation;

	clock_gettime(CLOCK_MONOTONIC, &time);
	generation = (uint32_t)time.tv_nsec ^ (uint32_t)time.tv_sec ^
		(uint32_t)getpid() * 2654435761u;
	return generation ? generation : 1;
}

static int connect_host(struct broker *broker)
{
	struct sockaddr_in address;
	struct copresent_open open_record;
	uint64_t gpa[COPRESENT_REGION_PAGES];
	char ack;
	int one = 1;

	if (region_pfns(broker->allocation, COPRESENT_REGION_BYTES, gpa,
			COPRESENT_REGION_PAGES) != 0)
		return -1;
	broker->control = socket(AF_INET, SOCK_STREAM, 0);
	if (broker->control < 0)
		return -1;
	memset(&address, 0, sizeof(address));
	address.sin_family = AF_INET;
	address.sin_port = htons(COPRESENT_CONTROL_PORT);
	address.sin_addr.s_addr = inet_addr(COPRESENT_HOST_ADDR);
	if (connect(broker->control, (struct sockaddr *)&address,
		    sizeof(address)) != 0) {
		perror("moco-present: connect host");
		return -1;
	}
	setsockopt(broker->control, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

	open_record.magic = COPRESENT_MAGIC;
	open_record.version = COPRESENT_VERSION;
	open_record.generation = broker->generation;
	open_record.npages = COPRESENT_REGION_PAGES;
	if (write_all(broker->control, &open_record, sizeof(open_record)) != 0 ||
	    write_all(broker->control, gpa, sizeof(gpa)) != 0 ||
	    read(broker->control, &ack, 1) != 1 || ack != 1) {
		fprintf(stderr, "moco-present: host refused metadata ring\n");
		return -1;
	}
	return 0;
}

static int ring_write(struct broker *broker,
		      const struct copresent_record *record)
{
	struct copresent_region *region = broker->region;
	uint32_t head;
	unsigned int waits = 0;
	int rc = 0;

	pthread_mutex_lock(&broker->tx_lock);
	for (;;) {
		uint32_t tail;

		head = region->tx_head;
		tail = load_acquire(&region->tx_tail);
		if ((uint32_t)(head - tail) < COPRESENT_RING_SLOTS)
			break;
		if (region->closed || ++waits > 100000) {
			rc = -1;
			goto out;
		}
		usleep(50);
	}
	region->tx[head % COPRESENT_RING_SLOTS] = *record;
	__atomic_thread_fence(__ATOMIC_RELEASE);
	store_release(&region->tx_head, head + 1);
	region->kick++;
	region->guest_heartbeat++;
out:
	pthread_mutex_unlock(&broker->tx_lock);
	return rc;
}

static int client_send(struct broker_client *client,
		       const struct copresent_record *record)
{
	int rc;

	pthread_mutex_lock(&client->send_lock);
	rc = client->fd >= 0 && send(client->fd, record, sizeof(*record),
		MSG_NOSIGNAL) == sizeof(*record) ? 0 : -1;
	pthread_mutex_unlock(&client->send_lock);
	return rc;
}

static struct broker_client *find_client(struct broker *broker,
					 uint32_t connection)
{
	struct broker_client *client;

	for (client = broker->clients; client; client = client->next)
		if (client->connection == connection)
			return client;
	return NULL;
}

static struct broker_buffer *find_buffer(struct broker_client *client,
					 uint32_t id)
{
	unsigned int i;

	for (i = 0; i < BROKER_MAX_BUFFERS; i++)
		if (client->buffers[i].active && client->buffers[i].id == id)
			return &client->buffers[i];
	return NULL;
}

static struct broker_buffer *alloc_buffer(struct broker_client *client)
{
	unsigned int i;

	for (i = 0; i < BROKER_MAX_BUFFERS; i++)
		if (!client->buffers[i].active)
			return &client->buffers[i];
	return NULL;
}

static void close_gem(struct broker *broker, struct broker_buffer *buffer)
{
	struct drm_gem_close close_record;

	if (!buffer->gem_handle)
		return;
	memset(&close_record, 0, sizeof(close_record));
	close_record.handle = buffer->gem_handle;
	ioctl(broker->drm_fd, DRM_IOCTL_GEM_CLOSE, &close_record);
	memset(buffer, 0, sizeof(*buffer));
}

static void *response_thread(void *opaque)
{
	struct broker *broker = opaque;
	struct copresent_region *region = broker->region;

	while (!region->closed) {
		uint32_t head = load_acquire(&region->rx_head);
		uint32_t tail = region->rx_tail;

		if (head == tail) {
			struct pollfd control = { .fd = broker->control,
				.events = POLLIN | POLLHUP | POLLERR };

			if (poll(&control, 1, 1) > 0 &&
			    (control.revents & (POLLHUP | POLLERR)))
				break;
			region->guest_heartbeat++;
			continue;
		}
		if ((uint32_t)(head - tail) > COPRESENT_RING_SLOTS) {
			fprintf(stderr, "moco-present: corrupt release occupancy\n");
			break;
		}
		while (tail != head) {
			struct copresent_record response =
				region->rx[tail % COPRESENT_RING_SLOTS];
			struct broker_client *client;

			__atomic_thread_fence(__ATOMIC_ACQUIRE);
			store_release(&region->rx_tail, ++tail);
			pthread_mutex_lock(&broker->clients_lock);
			client = find_client(broker, response.connection);
			if (client) {
				struct broker_buffer *buffer;

				pthread_mutex_lock(&client->state_lock);
				buffer = find_buffer(client, response.buffer);
				if (buffer &&
				    ((response.opcode == COPRESENT_UNBOUND) ||
				     (response.opcode == COPRESENT_BOUND &&
				      response.status != COPRESENT_OK)))
					close_gem(broker, buffer);
				pthread_mutex_unlock(&client->state_lock);
				client_send(client, &response);
			}
			pthread_mutex_unlock(&broker->clients_lock);
		}
	}
	region->closed = 1;
	return NULL;
}

static int receive_record_fd(int socket_fd, struct copresent_record *record,
			     int *received_fd)
{
	struct iovec iov = { .iov_base = record, .iov_len = sizeof(*record) };
	char control[CMSG_SPACE(sizeof(int))];
	struct msghdr message;
	struct cmsghdr *cmsg;
	ssize_t got;

	memset(&message, 0, sizeof(message));
	memset(control, 0, sizeof(control));
	message.msg_iov = &iov;
	message.msg_iovlen = 1;
	message.msg_control = control;
	message.msg_controllen = sizeof(control);
	got = recvmsg(socket_fd, &message, 0);
	if (got <= 0)
		return -1;
	if (got != sizeof(*record) || (message.msg_flags & MSG_TRUNC))
		return -1;
	*received_fd = -1;
	for (cmsg = CMSG_FIRSTHDR(&message); cmsg;
	     cmsg = CMSG_NXTHDR(&message, cmsg)) {
		if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS &&
		    cmsg->cmsg_len >= CMSG_LEN(sizeof(int))) {
			memcpy(received_fd, CMSG_DATA(cmsg), sizeof(int));
			break;
		}
	}
	return 0;
}

static int bind_buffer(struct broker_client *client,
		       struct copresent_record *record, int dma_buf)
{
	struct broker *broker = client->broker;
	struct drm_prime_handle prime;
	struct drm_virtgpu_resource_info info;
	struct broker_buffer *buffer;

	if (dma_buf < 0 || record->xid == 0 || record->width == 0 ||
	    record->height == 0 || find_buffer(client, record->buffer))
		return -1;
	buffer = alloc_buffer(client);
	if (!buffer)
		return -1;
	memset(&prime, 0, sizeof(prime));
	prime.fd = dma_buf;
	if (ioctl(broker->drm_fd, DRM_IOCTL_PRIME_FD_TO_HANDLE, &prime) != 0) {
		perror("moco-present: PRIME_FD_TO_HANDLE");
		return -1;
	}
	memset(&info, 0, sizeof(info));
	info.bo_handle = prime.handle;
	if (ioctl(broker->drm_fd, DRM_IOCTL_VIRTGPU_RESOURCE_INFO, &info) != 0 ||
	    info.res_handle == 0) {
		perror("moco-present: VIRTGPU_RESOURCE_INFO");
		buffer->gem_handle = prime.handle;
		close_gem(broker, buffer);
		return -1;
	}
	buffer->active = 1;
	buffer->id = record->buffer;
	buffer->gem_handle = prime.handle;
	buffer->resource_id = info.res_handle;
	buffer->xid = record->xid;
	buffer->width = record->width;
	buffer->height = record->height;
	record->resource_id = info.res_handle;
	return 0;
}

static int prepare_buffer_record(struct broker_client *client,
				 struct copresent_record *record)
{
	struct broker_buffer *buffer = find_buffer(client, record->buffer);

	if (!buffer || buffer->unbind_pending)
		return -1;
	record->resource_id = buffer->resource_id;
	record->xid = buffer->xid;
	record->width = buffer->width;
	record->height = buffer->height;
	return 0;
}

static void send_local_error(struct broker_client *client,
			     const struct copresent_record *request,
			     uint32_t status)
{
	struct copresent_record response = *request;

	response.opcode = COPRESENT_ERROR;
	response.status = status;
	response.generation = client->broker->generation;
	response.connection = client->connection;
	client_send(client, &response);
}

static void *client_thread(void *opaque)
{
	struct broker_client *client = opaque;
	struct broker *broker = client->broker;

	for (;;) {
		struct copresent_record record;
		int received_fd = -1;
		int ok = 0;

		if (receive_record_fd(client->fd, &record, &received_fd) != 0)
			break;
		if (record.version != COPRESENT_VERSION) {
			send_local_error(client, &record, COPRESENT_BAD_RECORD);
			if (received_fd >= 0)
				close(received_fd);
			continue;
		}
		record.generation = broker->generation;
		record.connection = client->connection;
		record.status = 0;

		switch (record.opcode) {
		case COPRESENT_BIND:
			pthread_mutex_lock(&client->state_lock);
			ok = bind_buffer(client, &record, received_fd);
			pthread_mutex_unlock(&client->state_lock);
			break;
		case COPRESENT_PRESENT:
			if (received_fd >= 0) {
				struct pollfd fence = { .fd = received_fd,
					.events = POLLIN | POLLERR };

				ok = poll(&fence, 1, 5000) > 0 ? 0 : -1;
			} else {
				ok = -1;
			}
			if (ok == 0) {
				pthread_mutex_lock(&client->state_lock);
				ok = prepare_buffer_record(client, &record);
				pthread_mutex_unlock(&client->state_lock);
			}
			if (ok == 0)
				record.flags |= COPRESENT_F_ACQUIRE_WAITED;
			break;
		case COPRESENT_UNBIND: {
			struct broker_buffer *buffer;

			pthread_mutex_lock(&client->state_lock);
			buffer = find_buffer(client, record.buffer);
			ok = prepare_buffer_record(client, &record);
			if (ok == 0)
				buffer->unbind_pending = 1;
			pthread_mutex_unlock(&client->state_lock);
			break;
		}
		default:
			ok = -1;
			break;
		}
		if (received_fd >= 0)
			close(received_fd);
		if (ok != 0 || ring_write(broker, &record) != 0)
			send_local_error(client, &record, COPRESENT_BAD_RECORD);
	}

	/* Retain imported GEM handles until the host acknowledges every UNBIND. */
	{
		unsigned int i;

		pthread_mutex_lock(&client->state_lock);
		for (i = 0; i < BROKER_MAX_BUFFERS; i++) {
			struct broker_buffer *buffer = &client->buffers[i];
			struct copresent_record record;

			if (!buffer->active || buffer->unbind_pending)
				continue;
			memset(&record, 0, sizeof(record));
			record.opcode = COPRESENT_UNBIND;
			record.version = COPRESENT_VERSION;
			record.generation = broker->generation;
			record.connection = client->connection;
			record.buffer = buffer->id;
			record.xid = buffer->xid;
			record.resource_id = buffer->resource_id;
			record.width = buffer->width;
			record.height = buffer->height;
			if (ring_write(broker, &record) == 0)
				buffer->unbind_pending = 1;
		}
		pthread_mutex_unlock(&client->state_lock);
	}
	pthread_mutex_lock(&client->send_lock);
	close(client->fd);
	client->fd = -1;
	pthread_mutex_unlock(&client->send_lock);
	return NULL;
}

static int make_local_listener(void)
{
	struct sockaddr_un address;
	int fd;

	mkdir("/run", 0755);
	unlink(COPRESENT_LOCAL_PATH);
	fd = socket(AF_UNIX, SOCK_SEQPACKET, 0);
	if (fd < 0)
		return -1;
	memset(&address, 0, sizeof(address));
	address.sun_family = AF_UNIX;
	strncpy(address.sun_path, COPRESENT_LOCAL_PATH,
		sizeof(address.sun_path) - 1);
	if (bind(fd, (struct sockaddr *)&address, sizeof(address)) != 0 ||
	    chmod(COPRESENT_LOCAL_PATH, 0666) != 0 || listen(fd, 16) != 0) {
		close(fd);
		return -1;
	}
	return fd;
}

int main(void)
{
	struct broker broker;
	pthread_t responses;

	memset(&broker, 0, sizeof(broker));
	broker.control = -1;
	broker.listen_fd = -1;
	broker.drm_fd = -1;
	if (geteuid() != 0) {
		fprintf(stderr, "moco-present: broker must run as root for pagemap\n");
		return 1;
	}
	signal(SIGPIPE, SIG_IGN);
	pthread_mutex_init(&broker.tx_lock, NULL);
	pthread_mutex_init(&broker.clients_lock, NULL);
	broker.generation = make_generation();
	broker.next_connection = 1;
	broker.allocation = mmap(NULL, COPRESENT_REGION_BYTES,
		PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE,
		-1, 0);
	if (broker.allocation == MAP_FAILED) {
		perror("moco-present: mmap");
		return 1;
	}
	memset(broker.allocation, 0, COPRESENT_REGION_BYTES);
	broker.region = (struct copresent_region *)broker.allocation;
	broker.region->magic = COPRESENT_MAGIC;
	broker.region->version = COPRESENT_VERSION;
	broker.region->generation = broker.generation;
	broker.drm_fd = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
	if (broker.drm_fd < 0) {
		perror("moco-present: renderD128");
		return 1;
	}
	if (connect_host(&broker) != 0)
		return 1;
	broker.listen_fd = make_local_listener();
	if (broker.listen_fd < 0) {
		perror("moco-present: local listener");
		return 1;
	}
	if (pthread_create(&responses, NULL, response_thread, &broker) != 0)
		return 1;
	pthread_detach(responses);
	fprintf(stderr, "moco-present: generation %u, metadata ring up at %s\n",
		broker.generation, COPRESENT_LOCAL_PATH);

	while (!broker.region->closed) {
		struct broker_client *client;
		pthread_t thread;
		int fd = accept(broker.listen_fd, NULL, NULL);

		if (fd < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		client = calloc(1, sizeof(*client));
		if (!client) {
			close(fd);
			continue;
		}
		client->broker = &broker;
		client->fd = fd;
		client->connection = broker.next_connection++;
		if (client->connection == 0)
			client->connection = broker.next_connection++;
		pthread_mutex_init(&client->send_lock, NULL);
		pthread_mutex_init(&client->state_lock, NULL);
		pthread_mutex_lock(&broker.clients_lock);
		client->next = broker.clients;
		broker.clients = client;
		pthread_mutex_unlock(&broker.clients_lock);
		if (pthread_create(&thread, NULL, client_thread, client) != 0) {
			close(fd);
			client->fd = -1;
			continue;
		}
		pthread_detach(thread);
	}

	broker.region->closed = 1;
	if (broker.listen_fd >= 0)
		close(broker.listen_fd);
	if (broker.control >= 0)
		close(broker.control);
	if (broker.drm_fd >= 0)
		close(broker.drm_fd);
	unlink(COPRESENT_LOCAL_PATH);
	munlock(broker.allocation, COPRESENT_REGION_BYTES);
	munmap(broker.allocation, COPRESENT_REGION_BYTES);
	return 0;
}
