/*
 * The guest half of the X wire.
 *
 * Owns /tmp/.X11-unix/X0, so DISPLAY=:0 reaches it and nothing above has to
 * know: not libX11, not VirtualGL, not the application. Every X client in the
 * guest connects here believing it has found a local X server, and what it has
 * actually found is a pipe into host memory.
 *
 * Per connection: allocate a shared region, pin it, tell the host which
 * physical pages it landed on, then copy bytes between the client's socket and
 * the rings until one end goes away.
 *
 * Runs as root, which is not a preference. Since Linux 4.0 a process without
 * CAP_SYS_ADMIN reads /proc/self/pagemap as zeroes -- the present bit is
 * honest, the frame number is a refusal -- and the whole design rests on being
 * able to name the physical pages. Firefox could not do this; a systemd
 * service can, which is why the translation lives here and not in a library
 * loaded into the client.
 *
 *   cc -O2 -o coxwire coxwire-guest.c -lpthread
 */

#define _GNU_SOURCE

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <stdarg.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "coxwire.h"

/* The host, as slirp presents it. Only ever used for the control message. */
#define HOST_ADDR	"10.0.2.2"

#define SOCK_PATH	"/tmp/.X11-unix/X0"

static int verbose;

/*
 * Where the guest's share of a frame goes.
 *
 * At 1080p the daemon is busy for about 14 ms of a 38 ms frame; the other 24 ms
 * is in here somewhere and guessing at it has already cost two wrong fixes. So
 * this counts, rather than assumes: time inside read() and write(), the bytes
 * they moved, and time spent with nothing to do.
 *
 * Note what the read() cost actually is. libX11 writes the frame into the unix
 * socket (one copy, user to kernel) and this reads it out into the ring
 * (a second copy, kernel to guest RAM the host already maps). Two passes over
 * 8.3 MB per 1080p frame, on one processor shared with Windows.
 *
 * Plain non-atomic counters: several channel threads add to them, so the totals
 * are approximate under contention. They are being used to find a leg worth
 * tens of milliseconds, not to audit bytes.
 */
static unsigned long long g_rd_us, g_rd_bytes, g_rd_n;
static unsigned long long g_wr_us, g_wr_bytes, g_wr_n;
static unsigned long long g_idle_us;

static unsigned long long now_us(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (unsigned long long)ts.tv_sec * 1000000ULL +
	       (unsigned long long)(ts.tv_nsec / 1000);
}

static void vlog(const char *fmt, ...)
{
	va_list ap;

	if (!verbose)
		return;
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
}

/*
 * Publish a counter.
 *
 * The payload stores must be visible before the counter that advertises them,
 * or the reader follows a promise into memory the writer has not filled. On
 * x86 stores are not reordered with stores, so this costs nothing at runtime;
 * it exists to stop the compiler sinking the payload past the counter, which
 * it is entitled to do and will.
 */
static inline void store_release(volatile uint64_t *p, uint64_t v)
{
	__atomic_store_n(p, v, __ATOMIC_RELEASE);
}

static inline uint64_t load_acquire(volatile uint64_t *p)
{
	return __atomic_load_n(p, __ATOMIC_ACQUIRE);
}

static int write_all(int fd, const void *buf, size_t n)
{
	const char *p = buf;

	while (n) {
		ssize_t w = write(fd, p, n);

		if (w < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		if (w == 0)
			return -1;
		p += w;
		n -= (size_t)w;
	}
	return 0;
}

/*
 * Guest virtual to guest physical.
 *
 * The region is locked first: an unlocked page can be migrated or swapped
 * after this reads its frame number, and the host would then be reading from
 * whatever took its place -- which is not a subtle failure, it is the host
 * reading another process's memory and presenting it as somebody's window.
 */
static int region_pfns(void *addr, size_t bytes, uint64_t *out, uint32_t max,
		       uint32_t *npages_out)
{
	int fd;
	size_t page = (size_t)sysconf(_SC_PAGESIZE);
	uint32_t n = (uint32_t)((bytes + page - 1) / page);
	uint32_t i;

	if (n > max) {
		fprintf(stderr, "coxwire: region needs %u pages, limit is %u\n",
			n, max);
		return -1;
	}

	if (mlock(addr, bytes) != 0) {
		perror("coxwire: mlock");
		return -1;
	}

	fd = open("/proc/self/pagemap", O_RDONLY);
	if (fd < 0) {
		perror("coxwire: pagemap");
		return -1;
	}

	for (i = 0; i < n; i++) {
		uint64_t entry;
		off_t off = (off_t)(((uintptr_t)addr / page) + i) * 8;

		if (pread(fd, &entry, 8, off) != 8) {
			perror("coxwire: pagemap read");
			close(fd);
			return -1;
		}
		if (!(entry & (1ULL << 63))) {
			fprintf(stderr, "coxwire: page %u not present after"
				" mlock\n", i);
			close(fd);
			return -1;
		}

		out[i] = (entry & ((1ULL << 55) - 1)) * page;

		/*
		 * Zero is not an address, it is a refusal -- the kernel hiding
		 * physical addresses from a process that lacks CAP_SYS_ADMIN.
		 * Sending it on would hand the host a page list aimed at guest
		 * physical zero and the failure would surface far away, on the
		 * other side of a socket, as a resolve that went out of range.
		 */
		if (out[i] == 0) {
			fprintf(stderr, "coxwire: pagemap returned frame 0 for"
				" page %u -- this must run as root\n", i);
			close(fd);
			return -1;
		}
	}

	close(fd);
	*npages_out = n;
	return 0;
}

/* One X client. */
struct chan {
	int		     cfd;	/* the client's unix socket    */
	int		     ctl;	/* control connection to host  */
	unsigned char	    *region;	/* header + both rings         */
	struct coxwire_hdr  *hdr;
	unsigned char	    *tx;
	unsigned char	    *rx;
	uint32_t	     id;
};

static void chan_free(struct chan *c)
{
	if (c->hdr)
		c->hdr->closed = 1;

	/*
	 * The pages must outlive the host's last touch of them.
	 *
	 * The host's channel thread checks `closed` once per loop and can be
	 * mid-recv() into the RX ring when it is set; releasing the region on
	 * this side first hands those pages back to the kernel while the host
	 * is still writing -- which is not a coxwire bug, it is the host
	 * scribbling over whoever got the pages next. Same family as the net
	 * ring's retirement and the 0xD5.
	 *
	 * The host acknowledges by closing the control socket (its chan_free
	 * does so unconditionally), so wait here for that read to return EOF.
	 * Bounded: a dead host daemon must not pin this memory forever, and
	 * five seconds is generous for one loop iteration. On timeout the
	 * region is still released -- the alternative is a permanent leak per
	 * window against a peer that is gone.
	 */
	if (c->ctl >= 0 && c->hdr) {
		struct timeval tv = { .tv_sec = 5, .tv_usec = 0 };
		char b;

		setsockopt(c->ctl, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
		while (read(c->ctl, &b, 1) > 0)
			;	/* drain until EOF, error, or timeout */
	}

	if (c->region) {
		munlock(c->region, COXWIRE_REGION_BYTES);
		munmap(c->region, COXWIRE_REGION_BYTES);
	}
	if (c->ctl >= 0)
		close(c->ctl);
	if (c->cfd >= 0)
		close(c->cfd);
	free(c);
}

/*
 * Announce the region to the host and wait for it to be accepted.
 *
 * This is the only thing that ever crosses slirp, and only once: a header and
 * a couple of thousand page addresses, about 17 KB. At the measured 15.3 MiB/s
 * that is close to a millisecond, paid at window creation, and never again.
 */
static int announce(struct chan *c)
{
	struct sockaddr_in sa;
	struct coxwire_open op;
	uint64_t *gpa;
	uint32_t npages = 0;
	char ack = 0;
	int one = 1;

	gpa = malloc(COXWIRE_MAX_PAGES * sizeof(*gpa));
	if (!gpa)
		return -1;

	if (region_pfns(c->region, COXWIRE_REGION_BYTES, gpa,
			COXWIRE_MAX_PAGES, &npages) != 0) {
		free(gpa);
		return -1;
	}

	c->ctl = socket(AF_INET, SOCK_STREAM, 0);
	if (c->ctl < 0) {
		free(gpa);
		return -1;
	}

	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_port = htons(COXWIRE_PORT);
	sa.sin_addr.s_addr = inet_addr(HOST_ADDR);

	if (connect(c->ctl, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
		perror("coxwire: connect to host");
		free(gpa);
		return -1;
	}
	setsockopt(c->ctl, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

	op.magic = COXWIRE_MAGIC;
	op.chan = c->id;
	op.ring_bytes = COXWIRE_RING_BYTES;
	op.npages = npages;

	if (write_all(c->ctl, &op, sizeof(op)) != 0 ||
	    write_all(c->ctl, gpa, (size_t)npages * sizeof(*gpa)) != 0) {
		free(gpa);
		return -1;
	}
	free(gpa);

	/*
	 * Wait for the host to say it has mapped the pages. Without this the
	 * client's first request can be written into a ring nobody is reading
	 * yet, and the X server would see a connection that opens and then
	 * stalls for as long as the control message takes.
	 */
	if (read(c->ctl, &ack, 1) != 1 || ack != 1) {
		fprintf(stderr, "coxwire: host refused channel %u\n", c->id);
		return -1;
	}

	vlog("coxwire: channel %u up, %u pages\n", c->id, npages);
	return 0;
}

/* Copy from the client's socket into the TX ring, one pass. */
static int pump_tx(struct chan *c)
{
	struct coxwire_hdr *h = c->hdr;
	uint64_t head = h->tx_head;
	uint64_t tail = load_acquire(&h->tx_tail);
	uint32_t used = (uint32_t)(head - tail);
	uint32_t space = COXWIRE_RING_BYTES - used;
	uint32_t at, run;
	ssize_t r;

	if (space == 0)
		return 0;		/* full: the client blocks, correctly */

	at = (uint32_t)(head % COXWIRE_RING_BYTES);
	run = COXWIRE_RING_BYTES - at;
	if (run > space)
		run = space;

	{
		unsigned long long t0 = now_us();

		r = read(c->cfd, c->tx + at, run);
		g_rd_us += now_us() - t0;
		g_rd_n++;
		if (r > 0)
			g_rd_bytes += (unsigned long long)r;
	}
	if (r < 0)
		return (errno == EINTR || errno == EAGAIN) ? 0 : -1;
	if (r == 0)
		return -1;		/* client closed */

	store_release(&h->tx_head, head + (uint64_t)r);
	return 1;
}

/* Copy from the RX ring out to the client's socket, one pass. */
static int pump_rx(struct chan *c)
{
	struct coxwire_hdr *h = c->hdr;
	uint64_t head = load_acquire(&h->rx_head);
	uint64_t tail = h->rx_tail;
	uint32_t avail = (uint32_t)(head - tail);
	uint32_t at, run;
	ssize_t w;

	if (avail == 0)
		return 0;

	at = (uint32_t)(tail % COXWIRE_RING_BYTES);
	run = COXWIRE_RING_BYTES - at;
	if (run > avail)
		run = avail;

	{
		unsigned long long t0 = now_us();

		w = write(c->cfd, c->rx + at, run);
		g_wr_us += now_us() - t0;
		g_wr_n++;
		if (w > 0)
			g_wr_bytes += (unsigned long long)w;
	}
	if (w < 0)
		return (errno == EINTR || errno == EAGAIN) ? 0 : -1;

	store_release(&h->rx_tail, tail + (uint64_t)w);
	return 1;
}

/*
 * One thread per X client.
 *
 * Blocking would be wrong in both directions at once, so the socket is
 * non-blocking and the loop alternates. When neither side moved, sleep briefly
 * rather than spin: the guest has one processor and it is shared with Windows,
 * so a busy loop here is taken directly out of everything else the guest is
 * doing. The host can afford to spin because it polls on another core; the
 * guest cannot.
 */
static void *chan_thread(void *arg)
{
	struct chan *c = arg;
	int flags = fcntl(c->cfd, F_GETFL, 0);
	int idle = 0;

	fcntl(c->cfd, F_SETFL, flags | O_NONBLOCK);

	for (;;) {
		int a = pump_tx(c);
		int b = pump_rx(c);

		if (a < 0 || b < 0)
			break;
		if (c->hdr->closed)
			break;

		if (a == 0 && b == 0) {
			unsigned long long t0 = now_us();

			if (++idle > 64)
				usleep(200);
			else
				sched_yield();
			g_idle_us += now_us() - t0;
		} else {
			idle = 0;
			c->hdr->kick++;
		}

		{
			static unsigned long long last;
			unsigned long long n = now_us();

			if (verbose && n - last > 2000000ULL) {
				last = n;
				fprintf(stderr,
					"coxwire: read %llu us / %llu MB /"
					" %llu calls (%llu MB/s) | write %llu us"
					" / %llu MB | idle %llu us\n",
					g_rd_us, g_rd_bytes >> 20, g_rd_n,
					g_rd_us ? (g_rd_bytes / g_rd_us) : 0,
					g_wr_us, g_wr_bytes >> 20, g_idle_us);
				fflush(stderr);
				g_rd_us = g_rd_bytes = g_rd_n = 0;
				g_wr_us = g_wr_bytes = g_wr_n = 0;
				g_idle_us = 0;
			}
		}
	}

	vlog("coxwire: channel %u closing\n", c->id);
	chan_free(c);
	return NULL;
}

int main(int argc, char **argv)
{
	int lfd;
	struct sockaddr_un su;
	uint32_t next_id = 1;
	int i;

	for (i = 1; i < argc; i++)
		if (!strcmp(argv[i], "-v"))
			verbose = 1;

	signal(SIGPIPE, SIG_IGN);

	if (geteuid() != 0) {
		fprintf(stderr, "coxwire: must run as root -- pagemap hides"
			" physical addresses otherwise\n");
		return 1;
	}

	mkdir("/tmp/.X11-unix", 01777);
	unlink(SOCK_PATH);

	lfd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (lfd < 0) {
		perror("coxwire: socket");
		return 1;
	}

	memset(&su, 0, sizeof(su));
	su.sun_family = AF_UNIX;
	strncpy(su.sun_path, SOCK_PATH, sizeof(su.sun_path) - 1);

	if (bind(lfd, (struct sockaddr *)&su, sizeof(su)) != 0) {
		perror("coxwire: bind " SOCK_PATH);
		return 1;
	}
	if (chmod(SOCK_PATH, 0777) != 0)
		perror("coxwire: chmod");
	if (listen(lfd, 16) != 0) {
		perror("coxwire: listen");
		return 1;
	}

	fprintf(stderr, "coxwire: listening on %s, host %s:%d,"
		" %u KB rings\n", SOCK_PATH, HOST_ADDR, COXWIRE_PORT,
		COXWIRE_RING_BYTES / 1024);

	for (;;) {
		struct chan *c;
		pthread_t th;
		int cfd = accept(lfd, NULL, NULL);

		if (cfd < 0) {
			if (errno == EINTR)
				continue;
			perror("coxwire: accept");
			break;
		}

		c = calloc(1, sizeof(*c));
		if (!c) {
			close(cfd);
			continue;
		}
		c->cfd = cfd;
		c->ctl = -1;
		c->id = next_id++;

		/*
		 * MAP_POPULATE so the pages exist before pagemap is asked
		 * about them; mlock in region_pfns then keeps them still.
		 */
		c->region = mmap(NULL, COXWIRE_REGION_BYTES,
				 PROT_READ | PROT_WRITE,
				 MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE,
				 -1, 0);
		if (c->region == MAP_FAILED) {
			perror("coxwire: mmap");
			c->region = NULL;
			chan_free(c);
			continue;
		}

		memset(c->region, 0, COXWIRE_PAGE);
		c->hdr = (struct coxwire_hdr *)c->region;
		c->hdr->ring_bytes = COXWIRE_RING_BYTES;
		c->tx = c->region + COXWIRE_TX_OFF;
		c->rx = c->region + COXWIRE_RX_OFF;

		if (announce(c) != 0) {
			chan_free(c);
			continue;
		}

		if (pthread_create(&th, NULL, chan_thread, c) != 0) {
			perror("coxwire: pthread_create");
			chan_free(c);
			continue;
		}
		pthread_detach(th);
	}

	close(lfd);
	unlink(SOCK_PATH);
	return 0;
}
