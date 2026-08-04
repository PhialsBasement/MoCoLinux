/*
 * A minimal X11 client, spoken on the wire, from the host side.
 *
 * This is how a frame rendered on the Windows GPU reaches the window the guest
 * thinks it owns -- and the reason it lives here rather than in the guest is
 * arithmetic. The guest's X connection goes through slirp at about 16 MB/s; a
 * 1024x768 frame is 3 MB, so presenting from inside the guest caps out around
 * five frames a second and 1080p is under two. The daemon, by contrast, is on
 * the same machine as the X server: it connects to 127.0.0.1:6000, which is
 * loopback, and the pixels never enter the guest at all. What crosses the ring
 * is a resource id, a window id and a sequence number.
 *
 * Xlib is not used because there is no Xlib here worth linking -- this is a
 * mingw program and the protocol is small. Only four things are needed:
 * connect and read the setup reply, allocate an id, create a GC, and PutImage.
 * Everything else X can do is somebody else's problem.
 *
 * Written to be built for Linux as well, so it can be debugged against a real
 * X server on the development machine before it goes anywhere near the box --
 * this project's own rule about never-executed code.
 */

#ifdef _WIN32
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  define CLOSESOCK closesocket
typedef SOCKET sockfd_t;
#  define BAD_SOCK INVALID_SOCKET
#else
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <netinet/tcp.h>
#  include <arpa/inet.h>
#  include <unistd.h>
#  define CLOSESOCK close
typedef int sockfd_t;
#  define BAD_SOCK (-1)
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "x11.h"

/* X protocol opcodes, from the core protocol specification. */
#define X_CreateWindow		 1
#define X_MapWindow		 8
#define X_GetInputFocus		43
#define X_CreateGC		55
#define X_FreeGC		60
#define X_PutImage		72

#define ZPixmap			 2

struct x11_conn {
	sockfd_t fd;
	uint32_t id_base;
	uint32_t id_mask;
	uint32_t next_id;
	uint32_t root;
	uint32_t root_visual;
	uint8_t	 root_depth;
	uint16_t seq;
	/* The largest request the server will accept, in 4-byte units. */
	uint32_t max_request;
	int	 image_byte_order;
	int	 bitmap_pad;
};

static int sock_send(sockfd_t fd, const void *buf, size_t n)
{
	const char *p = buf;

	while (n) {
		int w = send(fd, p, (int)n, 0);

		if (w <= 0)
			return -1;
		p += w;
		n -= (size_t)w;
	}
	return 0;
}

static int sock_recv(sockfd_t fd, void *buf, size_t n)
{
	char *p = buf;

	while (n) {
		int r = recv(fd, p, (int)n, 0);

		if (r <= 0)
			return -1;
		p += r;
		n -= (size_t)r;
	}
	return 0;
}

/*
 * The connection setup, which is the only genuinely fiddly part of the
 * protocol: a fixed header, then a variable block of vendor string, pixmap
 * formats and screens, each padded to four bytes. The fields that matter here
 * are the resource id base and mask (we invent ids from those), and the first
 * screen's root window, depth and visual.
 */
struct x11_conn *x11_connect(const char *host, int display)
{
	struct x11_conn *c;
	struct sockaddr_in sa;
	unsigned char setup[12];
	unsigned char hdr[8];
	unsigned char *body;
	uint16_t extra_len;
	int one = 1;

	c = calloc(1, sizeof(*c));
	if (!c)
		return NULL;

	c->fd = socket(AF_INET, SOCK_STREAM, 0);
	if (c->fd == BAD_SOCK) {
		free(c);
		return NULL;
	}

	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_port   = htons((unsigned short)(6000 + display));
	sa.sin_addr.s_addr = inet_addr(host ? host : "127.0.0.1");

	if (connect(c->fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
		CLOSESOCK(c->fd);
		free(c);
		return NULL;
	}

	/* Nagle off: presents are small writes that must not wait for company. */
	setsockopt(c->fd, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof(one));

	/*
	 * No authorisation data. xstart1142.bat runs the server with -ac, so
	 * it accepts any client; sending a cookie we do not have would be
	 * refused, and asking for one would mean parsing .Xauthority from a
	 * Windows profile.
	 */
	memset(setup, 0, sizeof(setup));
	setup[0] = (0x1234 == htons(0x1234)) ? 'B' : 'l';	/* byte order */
	setup[2] = 11;						/* major */
	setup[4] = 0;						/* minor */

	if (sock_send(c->fd, setup, sizeof(setup)) != 0)
		goto fail;

	if (sock_recv(c->fd, hdr, sizeof(hdr)) != 0)
		goto fail;

	if (hdr[0] != 1) {			/* 1 = success */
		fprintf(stderr, "x11: server refused the connection (%u)\n", hdr[0]);
		goto fail;
	}

	extra_len = (uint16_t)(hdr[6] | (hdr[7] << 8));	/* in 4-byte units */
	body = malloc((size_t)extra_len * 4);
	if (!body)
		goto fail;
	if (sock_recv(c->fd, body, (size_t)extra_len * 4) != 0) {
		free(body);
		goto fail;
	}

	/*
	 * Offsets inside the setup reply body, counted from the spec. The
	 * vendor string and the pixmap formats sit between the fixed part and
	 * the screens, so the first screen has to be found by stepping over
	 * them rather than by a constant.
	 */
	{
		uint32_t rel_base = *(uint32_t *)(body + 4);
		uint32_t rel_mask = *(uint32_t *)(body + 8);
		uint16_t vendor_len = *(uint16_t *)(body + 16);
		uint16_t max_req = *(uint16_t *)(body + 18);
		uint8_t	 num_formats = body[21];
		unsigned char *screen;

		c->id_base = rel_base;
		c->id_mask = rel_mask;
		c->max_request = max_req;
		c->image_byte_order = body[22];
		c->bitmap_pad = body[25];

		screen = body + 32 + ((vendor_len + 3) & ~3) + 8 * num_formats;

		c->root	       = *(uint32_t *)(screen + 0);
		c->root_visual = *(uint32_t *)(screen + 32);
		c->root_depth  = screen[38];
	}

	free(body);
	c->next_id = 0;
	c->seq = 1;
	return c;

fail:
	CLOSESOCK(c->fd);
	free(c);
	return NULL;
}

void x11_close(struct x11_conn *c)
{
	if (!c)
		return;
	CLOSESOCK(c->fd);
	free(c);
}

uint32_t x11_alloc_id(struct x11_conn *c)
{
	uint32_t id = c->id_base | ((c->next_id << 0) & c->id_mask);

	c->next_id++;
	return id;
}

uint32_t x11_root(struct x11_conn *c)		{ return c->root; }
uint8_t	 x11_root_depth(struct x11_conn *c)	{ return c->root_depth; }

int x11_create_gc(struct x11_conn *c, uint32_t gc, uint32_t drawable)
{
	uint32_t req[4];

	req[0] = X_CreateGC | (4u << 16);	/* opcode, request length */
	req[1] = gc;
	req[2] = drawable;
	req[3] = 0;				/* value mask: no values */

	c->seq++;
	return sock_send(c->fd, req, sizeof(req));
}

/*
 * A round trip, used as a barrier.
 *
 * X is asynchronous, so a PutImage that is going to fail does so somewhere
 * later and asynchronously. GetInputFocus is the traditional cheap request
 * with a reply: when its reply arrives, everything before it has been
 * processed, and any error it provoked has already been sent.
 */
int x11_sync(struct x11_conn *c)
{
	uint32_t req[1];
	unsigned char reply[32];

	req[0] = X_GetInputFocus | (1u << 16);
	c->seq++;
	if (sock_send(c->fd, req, sizeof(req)) != 0)
		return -1;

	for (;;) {
		if (sock_recv(c->fd, reply, sizeof(reply)) != 0)
			return -1;
		if (reply[0] == 1)		/* a reply: our sync */
			return 0;
		if (reply[0] == 0) {		/* an error */
			fprintf(stderr, "x11: error code %u, major %u, minor %u\n",
				reply[1], reply[10], (unsigned)(reply[8] | (reply[9] << 8)));
			return -1;
		}
		/* anything else is an event; this client asked for none, but
		 * the server may still send some. Ignore and keep reading. */
	}
}

/*
 * PutImage, in bands.
 *
 * A core request carries at most (max_request * 4) bytes, which on a typical
 * server is 262140 -- less than a third of a 1024x768 frame. BIG-REQUESTS
 * would lift that, but it is an extension that has to be negotiated, and
 * banding is simpler and works everywhere: each band is a PutImage of as many
 * whole scanlines as will fit.
 */
int x11_put_image(struct x11_conn *c, uint32_t drawable, uint32_t gc,
		  const void *pixels, int width, int height, int dst_x, int dst_y,
		  int depth)
{
	const unsigned char *src = pixels;
	uint32_t stride = (uint32_t)width * 4;
	uint32_t max_bytes = (c->max_request - 8) * 4;
	int rows_per_band = (int)(max_bytes / stride);
	int y;

	if (rows_per_band < 1)
		return -1;

	for (y = 0; y < height; y += rows_per_band) {
		int rows = height - y;
		uint32_t bytes, words;
		uint32_t req[6];

		if (rows > rows_per_band)
			rows = rows_per_band;

		bytes = stride * (uint32_t)rows;
		words = (bytes + 3) / 4;

		req[0] = X_PutImage | ((uint32_t)ZPixmap << 8) | ((6 + words) << 16);
		req[1] = drawable;
		req[2] = gc;
		req[3] = ((uint32_t)width) | (((uint32_t)rows) << 16);
		req[4] = ((uint32_t)(dst_x) & 0xffff) |
			 (((uint32_t)(dst_y + y) & 0xffff) << 16);
		req[5] = ((uint32_t)depth << 8);		/* left-pad 0 */

		if (sock_send(c->fd, req, sizeof(req)) != 0)
			return -1;
		if (sock_send(c->fd, src + (size_t)stride * y, bytes) != 0)
			return -1;

		/* pad to a 4-byte boundary */
		if (words * 4 != bytes) {
			static const char pad[4] = { 0, 0, 0, 0 };

			if (sock_send(c->fd, pad, words * 4 - bytes) != 0)
				return -1;
		}
		c->seq++;
	}

	return 0;
}

/* A window of our own, for the standalone test below. */
int x11_create_window(struct x11_conn *c, uint32_t wid, int width, int height)
{
	uint32_t req[10];

	req[0] = X_CreateWindow | ((uint32_t)c->root_depth << 8) | (10u << 16);
	req[1] = wid;
	req[2] = c->root;
	req[3] = 0;					/* x, y */
	req[4] = ((uint32_t)width) | (((uint32_t)height) << 16);
	req[5] = 0;					/* border, class */
	req[6] = c->root_visual;
	req[7] = 0x00000002;				/* CWBackPixel */
	req[8] = 0x00000000;				/* black */
	req[9] = 0;

	c->seq++;
	if (sock_send(c->fd, req, 9 * 4) != 0)
		return -1;

	{
		uint32_t map[2];

		map[0] = X_MapWindow | (2u << 16);
		map[1] = wid;
		c->seq++;
		return sock_send(c->fd, map, sizeof(map));
	}
}

#ifdef X11_STANDALONE_TEST
/*
 * Debugged here before it goes near the box.
 *
 *   cc -DX11_STANDALONE_TEST x11.c -o x11test && ./x11test 127.0.0.1 0
 *
 * Opens a window on whatever X server is named and fills it with a gradient,
 * which is exactly what the daemon will do with a real frame -- the only
 * difference being where the pixels came from.
 */
int main(int argc, char **argv)
{
	const char *host = (argc > 1) ? argv[1] : "127.0.0.1";
	int display = (argc > 2) ? atoi(argv[2]) : 0;
	struct x11_conn *c;
	uint32_t wid, gc;
	int w = 640, h = 480, x, y;
	unsigned char *img;

#ifdef _WIN32
	{
		WSADATA wsa;
		WSAStartup(MAKEWORD(2, 2), &wsa);
	}
#endif

	c = x11_connect(host, display);
	if (!c) {
		printf("could not connect to %s:%d\n", host, display);
		return 1;
	}
	printf("connected: root 0x%x, depth %u, max request %u words\n",
	       x11_root(c), x11_root_depth(c), c->max_request);

	wid = x11_alloc_id(c);
	gc  = x11_alloc_id(c);

	if (x11_create_window(c, wid, w, h) != 0) { printf("create failed\n"); return 1; }
	if (x11_create_gc(c, gc, wid) != 0)	  { printf("gc failed\n"); return 1; }
	if (x11_sync(c) != 0)			  { printf("sync failed\n"); return 1; }

	img = malloc((size_t)w * h * 4);
	for (y = 0; y < h; y++)
		for (x = 0; x < w; x++) {
			unsigned char *p = img + ((size_t)y * w + x) * 4;

			p[0] = (unsigned char)(x * 255 / w);	/* B */
			p[1] = (unsigned char)(y * 255 / h);	/* G */
			p[2] = 0x80;				/* R */
			p[3] = 0xff;
		}

	if (x11_put_image(c, wid, gc, img, w, h, 0, 0, c->root_depth) != 0) {
		printf("PutImage failed\n");
		return 1;
	}
	if (x11_sync(c) != 0) { printf("sync after PutImage failed\n"); return 1; }

	printf("put a %dx%d gradient; window stays up for 10 s\n", w, h);
#ifdef _WIN32
	Sleep(10000);
#else
	sleep(10);
#endif
	x11_close(c);
	return 0;
}
#endif
