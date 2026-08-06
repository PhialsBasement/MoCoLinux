/*
 * The host half of the X wire.
 *
 * A channel arrives on the control port as a list of guest-physical pages. The
 * daemon already maps the whole of guest RAM through R3's windows, so those
 * pages become pointers of its own and the rings are read and written in
 * place -- the bytes of an XPutImage are never copied out of the guest, they
 * are handed straight to a socket that ends at VcXsrv on the same machine.
 *
 * Why per page rather than per run: the windows are 8 MB slices, adjacent in
 * guest-physical but not in host-virtual, and the guest's region is ordinary
 * scattered anonymous memory besides. A page is the largest unit guaranteed
 * contiguous on both sides, so the ring walk is page-granular and any run that
 * crosses a page boundary is split. resolve_run() exists for exactly this and
 * getting it wrong is what made every large ATTACH_BACKING fail.
 *
 * Each channel gets a thread that spins. That is affordable here and nowhere
 * else: the daemon is on a different physical core from the guest, which is
 * the same trick that makes the GPU ring's round trip 3 microseconds. The
 * guest's own shim must not spin, and does not.
 */

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../../../user/coxwire/coxwire.h"

/* Provided by cogpu-daemon.c: guest-physical to a pointer we can use, and how
 * much of a run is reachable as one. */
extern void *resolve_gpa(void *ctx, uint64_t gpa, uint32_t len);
extern void logline(const char *fmt, ...);

/* Where VcXsrv listens. Loopback on this machine -- the whole point. */
#define XSERVER_PORT	6000

/*
 * What the X server costs us, and why this is no longer measured here.
 *
 * Timing the send() answered the question it was asked -- 513857 would-blocks
 * against 4830 useful sends, VcXsrv draining at 135-150 MB/s against the
 * 500 MB/s that 1080p60 needs -- and then quietly became the bottleneck
 * itself. xw_now_us() called QueryPerformanceFrequency and
 * QueryPerformanceCounter, twice per send(), inside a loop that issues half a
 * million sends every two seconds: about a million clock calls per second
 * added to the hottest path in the daemon.
 *
 * It cost 1080p glxgears 25.9 -> 18.7 fps, and I spent a reboot and a round of
 * Xorg-blaming looking for that regression somewhere else. If this needs
 * measuring again, sample it -- every thousandth call, with the frequency read
 * once at startup -- rather than timing every one.
 */

struct xchan {
	SOCKET		     ctl;
	SOCKET		     x;		/* to VcXsrv			*/
	struct coxwire_hdr  *hdr;
	unsigned char	   **page;	/* host pointer per guest page	*/
	uint32_t	    *runlen;	/* contiguous bytes from each page */
	uint32_t	     npages;
	uint32_t	     id;
	int		     tx_stalled;	/* socket full, waiting on VcXsrv */
};

/*
 * How far a page's host mapping runs before it stops being contiguous.
 *
 * The first version answered "to the end of this page" and that was the
 * measurement's ceiling, not the design's: every send() carried 4096 bytes, so
 * a 3 MB frame cost 768 syscalls and 1024x768 topped out at 28 fps. The pages
 * are scattered in guest-physical space, but scattered is not the same as
 * unlucky -- an 8 MB anonymous mapping under MAP_POPULATE comes back in long
 * physically contiguous runs, and transparent huge pages make many of them
 * 2 MB. Where two pages happen to be adjacent on both sides they can be sent
 * as one.
 *
 * Computed backwards once, at channel setup, so the hot path is a lookup.
 */
static void build_runs(struct xchan *c)
{
	uint32_t i = c->npages;

	c->runlen[i - 1] = COXWIRE_PAGE;
	while (i-- > 1) {
		if (c->page[i - 1] + COXWIRE_PAGE == c->page[i])
			c->runlen[i - 1] = COXWIRE_PAGE + c->runlen[i];
		else
			c->runlen[i - 1] = COXWIRE_PAGE;
	}
}

/*
 * Byte at `off` into the region, as a host pointer, and how many contiguous
 * bytes follow it.
 *
 * The run returned can now cross page boundaries, so it can also cross the
 * boundary between the two rings -- which it must never be allowed to do. The
 * callers clamp it to what is left before their ring wraps; that clamp was not
 * needed while runs were page-sized (both rings are page-aligned, so a page
 * could not straddle them) and is needed now.
 */
static unsigned char *at(struct xchan *c, uint32_t off, uint32_t *run)
{
	uint32_t pg = off / COXWIRE_PAGE;
	uint32_t in = off % COXWIRE_PAGE;

	if (pg >= c->npages) {
		*run = 0;
		return NULL;
	}
	*run = c->runlen[pg] - in;
	return c->page[pg] + in;
}

static void chan_free(struct xchan *c)
{
	if (!c)
		return;
	if (c->hdr)
		c->hdr->closed = 1;
	if (c->x != INVALID_SOCKET)
		closesocket(c->x);
	if (c->ctl != INVALID_SOCKET)
		closesocket(c->ctl);
	free(c->runlen);
	free(c->page);
	free(c);
}

static int recv_all(SOCKET s, void *buf, int n)
{
	char *p = buf;

	while (n > 0) {
		int r = recv(s, p, n, 0);

		if (r <= 0)
			return -1;
		p += r;
		n -= r;
	}
	return 0;
}

/*
 * Drain the guest's TX ring into VcXsrv.
 *
 * Bounded per pass so one very large PutImage cannot starve the reverse
 * direction: a 1080p frame is twice the ring, so it arrives as several passes
 * regardless, and interleaving replies matters more than finishing a frame in
 * one go.
 */
static int drain_tx(struct xchan *c)
{
	struct coxwire_hdr *h = c->hdr;
	uint64_t head = h->tx_head;		/* guest publishes */
	uint64_t tail = h->tx_tail;		/* ours            */
	uint32_t avail = (uint32_t)(head - tail);
	int moved = 0;

	c->tx_stalled = 0;
	MemoryBarrier();

	while (avail) {
		uint32_t idx = (uint32_t)(tail % COXWIRE_RING_BYTES);
		uint32_t off = COXWIRE_TX_OFF + idx;
		uint32_t to_wrap = COXWIRE_RING_BYTES - idx;
		uint32_t run = 0;
		unsigned char *p = at(c, off, &run);
		int w;

		if (!p)
			return -1;
		if (run > to_wrap)
			run = to_wrap;
		if (run > avail)
			run = avail;

		w = send(c->x, (const char *)p, (int)run, 0);
		if (w <= 0) {
			int e = WSAGetLastError();

			if (e == WSAEWOULDBLOCK) {
				c->tx_stalled = 1;
				break;
			}
			return -1;
		}

		tail += (uint32_t)w;
		avail -= (uint32_t)w;
		moved += w;

		/* Publish as we go: the guest can refill behind us rather than
		 * waiting for the whole burst to clear. */
		MemoryBarrier();
		h->tx_tail = tail;

		if (moved > 1 << 20)
			break;
	}

	return moved;
}

/* Fill the guest's RX ring from VcXsrv. */
static int fill_rx(struct xchan *c)
{
	struct coxwire_hdr *h = c->hdr;
	uint64_t head = h->rx_head;		/* ours            */
	uint64_t tail = h->rx_tail;		/* guest publishes */
	uint32_t used = (uint32_t)(head - tail);
	uint32_t space = COXWIRE_RING_BYTES - used;
	uint32_t idx, off, to_wrap, run = 0;
	unsigned char *p;
	int r;

	MemoryBarrier();

	if (!space)
		return 0;

	idx = (uint32_t)(head % COXWIRE_RING_BYTES);
	off = COXWIRE_RX_OFF + idx;
	to_wrap = COXWIRE_RING_BYTES - idx;
	p = at(c, off, &run);
	if (!p)
		return -1;
	if (run > to_wrap)
		run = to_wrap;
	if (run > space)
		run = space;

	r = recv(c->x, (char *)p, (int)run, 0);
	if (r == 0)
		return -1;			/* server closed */
	if (r < 0) {
		int e = WSAGetLastError();

		return (e == WSAEWOULDBLOCK) ? 0 : -1;
	}

	MemoryBarrier();
	h->rx_head = head + (uint32_t)r;
	return r;
}

static DWORD WINAPI chan_thread(LPVOID arg)
{
	struct xchan *c = arg;
	int idle = 0;

	for (;;) {
		int a = drain_tx(c);
		int b = fill_rx(c);

		if (a < 0 || b < 0)
			break;
		if (c->hdr->closed)
			break;

		if ((a == 0 && b == 0) || c->tx_stalled) {
			/*
			 * Spin. Measured, not assumed.
			 *
			 * Spinning here wastes syscalls extravagantly -- 513857
			 * would-blocks in two seconds against 4830 sends that
			 * moved anything -- and three attempts to stop it all
			 * made the frame rate worse, on a clean boot with the
			 * guest otherwise idle:
			 *
			 *     spin                      25.9 fps   x11perf 133/s
			 *     select, 2 ms timeout      19.9       x11perf 149/s
			 *     select, 250 us timeout    21.3
			 *     spin 3000 then select     18.0
			 *
			 * The reason is the workload. Under VGL_SPOIL=0 every
			 * frame is one request and one reply, so any wait added
			 * here lands on the frame directly, while a streaming
			 * client like x11perf only notices the wasted syscalls.
			 * Interactive applications are the point, so latency
			 * wins and the waste stays. Revisit only with a way to
			 * sleep that wakes in microseconds.
			 */

			/*
			 * Nothing on either side. Spin briefly -- the guest may
			 * be about to write -- then stand down, because an idle
			 * X connection must not hold a core hot and there can be
			 * one of these per window.
			 *
			 * A three-stage version of this (spin longer, then
			 * SwitchToThread, then sleep, with timeBeginPeriod(1)
			 * so the sleep was a millisecond rather than the 15.6 ms
			 * tick) was tried on the theory that the tick was the
			 * per-frame cost. It measured slower, twice.
			 */
			if (++idle > 20000)
				Sleep(1);
			else
				YieldProcessor();
		} else {
			idle = 0;
		}

	}

	logline("xwire: channel %u closed\n", c->id);
	chan_free(c);
	return 0;
}

/* Read one channel's announcement and map its pages. */
static struct xchan *accept_chan(SOCKET ctl)
{
	struct xchan *c;
	struct coxwire_open op;
	struct sockaddr_in sa;
	uint64_t *gpa = NULL;
	uint32_t i;
	u_long nb = 1;
	char ack = 0;

	c = calloc(1, sizeof(*c));
	if (!c)
		return NULL;
	c->ctl = ctl;
	c->x = INVALID_SOCKET;

	if (recv_all(ctl, &op, sizeof(op)) != 0)
		goto fail;

	if (op.magic != COXWIRE_MAGIC) {
		logline("xwire: bad magic 0x%08x, refusing\n", op.magic);
		goto fail;
	}
	if (op.npages == 0 || op.npages > COXWIRE_MAX_PAGES ||
	    op.ring_bytes != COXWIRE_RING_BYTES) {
		logline("xwire: channel %u: %u pages, ring %u -- refusing\n",
			op.chan, op.npages, op.ring_bytes);
		goto fail;
	}

	gpa = malloc((size_t)op.npages * sizeof(*gpa));
	if (!gpa)
		goto fail;
	if (recv_all(ctl, gpa, (int)(op.npages * sizeof(*gpa))) != 0)
		goto fail;

	c->page = calloc(op.npages, sizeof(*c->page));
	c->runlen = calloc(op.npages, sizeof(*c->runlen));
	if (!c->page || !c->runlen)
		goto fail;
	c->npages = op.npages;
	c->id = op.chan;

	for (i = 0; i < op.npages; i++) {
		void *h = resolve_gpa(NULL, gpa[i], COXWIRE_PAGE);

		if (!h) {
			logline("xwire: channel %u: page %u at 0x%llx is"
				" outside guest RAM\n", op.chan, i,
				(unsigned long long)gpa[i]);
			goto fail;
		}
		c->page[i] = h;
	}

	build_runs(c);
	c->hdr = (struct coxwire_hdr *)c->page[0];

	/* Now the X server, on loopback. */
	c->x = socket(AF_INET, SOCK_STREAM, 0);
	if (c->x == INVALID_SOCKET)
		goto fail;

	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_port = htons(XSERVER_PORT);
	sa.sin_addr.s_addr = inet_addr("127.0.0.1");

	if (connect(c->x, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
		logline("xwire: channel %u: cannot reach the X server on"
			" 127.0.0.1:%d (%d)\n", op.chan, XSERVER_PORT,
			WSAGetLastError());
		goto fail;
	}

	{
		int one = 1;

		setsockopt(c->x, IPPROTO_TCP, TCP_NODELAY,
			   (const char *)&one, sizeof(one));
	}
	ioctlsocket(c->x, FIONBIO, &nb);

	ack = 1;
	if (send(ctl, &ack, 1, 0) != 1)
		goto fail;

	free(gpa);
	{
		uint32_t big = 0, j;

		for (j = 0; j < c->npages; j++)
			if (c->runlen[j] > big)
				big = c->runlen[j];

		logline("xwire: channel %u up, %u pages, %u KB rings,"
			" longest contiguous run %u KB\n",
			op.chan, op.npages, COXWIRE_RING_BYTES / 1024,
			big / 1024);
	}
	return c;

fail:
	free(gpa);
	chan_free(c);
	return NULL;
}

static DWORD WINAPI listen_thread(LPVOID arg)
{
	SOCKET l = (SOCKET)(size_t)arg;

	for (;;) {
		SOCKET s = accept(l, NULL, NULL);
		struct xchan *c;
		HANDLE th;

		if (s == INVALID_SOCKET)
			break;

		c = accept_chan(s);
		if (!c)
			continue;

		th = CreateThread(NULL, 0, chan_thread, c, 0, NULL);
		if (!th) {
			chan_free(c);
			continue;
		}
		CloseHandle(th);
	}

	closesocket(l);
	return 0;
}

/*
 * Start listening. Called once from main; returns 0 if the wire is available,
 * and the guest simply keeps using slirp if it is not.
 */
int cogpu_xwire_start(void)
{
	SOCKET l;
	struct sockaddr_in sa;
	int one = 1;
	HANDLE th;
	WSADATA wsa;

	/*
	 * This daemon has never opened a socket before, so nothing has started
	 * Winsock for it. Without this every socket() returns INVALID_SOCKET
	 * and the wire simply never appears -- which is exactly how it failed
	 * the first time, silently, because the early return below had no log
	 * line of its own. It has one now: a path that gives up must say so.
	 */
	if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
		logline("xwire: WSAStartup failed (%d)\n", WSAGetLastError());
		return -1;
	}

	l = socket(AF_INET, SOCK_STREAM, 0);
	if (l == INVALID_SOCKET) {
		logline("xwire: socket() failed (%d)\n", WSAGetLastError());
		return -1;
	}

	setsockopt(l, SOL_SOCKET, SO_REUSEADDR, (const char *)&one,
		   sizeof(one));

	/*
	 * Loopback only. The guest reaches this port as 10.0.2.2, which slirp
	 * rewrites to this machine's loopback -- so the guest path needs
	 * nothing wider. Anything wider is a hole: a control connection names
	 * arbitrary guest-physical pages, and answering the RJ45 link would
	 * let another machine aim this daemon's writes into guest RAM.
	 */
	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_port = htons(COXWIRE_PORT);
	sa.sin_addr.s_addr = inet_addr("127.0.0.1");

	if (bind(l, (struct sockaddr *)&sa, sizeof(sa)) != 0 ||
	    listen(l, 8) != 0) {
		logline("xwire: cannot listen on %d (%d)\n", COXWIRE_PORT,
			WSAGetLastError());
		closesocket(l);
		return -1;
	}

	th = CreateThread(NULL, 0, listen_thread, (LPVOID)(size_t)l, 0, NULL);
	if (!th) {
		closesocket(l);
		return -1;
	}
	CloseHandle(th);

	logline("xwire: listening on %d, X server at 127.0.0.1:%d\n",
		COXWIRE_PORT, XSERVER_PORT);
	return 0;
}
