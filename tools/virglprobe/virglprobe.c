/*
 * virglprobe -- does virglrenderer render on this host's GPU?
 *
 * R2 of the GPU ladder, and the rung's whole question in one program: can
 * cross-compiled virglrenderer, driven through our own WGL winsys, decode a
 * virgl command stream and produce correct pixels on the GT 730.
 *
 * The plan proposed proving this by porting virglrenderer's vtest server to
 * Winsock and pointing guest Mesa at it over slirp. That estimate was wrong by
 * an order of magnitude -- vtest is not the 675-line vtest_server.c but nearly
 * 5,000 lines across a renderer, a shm layer and a threadpool, built on AF_UNIX,
 * memfd and eventfd. Days of porting for a component the plan itself calls
 * throwaway.
 *
 * So the same question is asked directly. This program IS the host half of
 * vtest, minus the socket: it hands virglrenderer a command stream we encode
 * ourselves and reads the result back. What it proves is exactly what R5 stands
 * on -- the winsys satisfies vrend, the decoder runs, the GPU draws, and the
 * pixels are right. What it does not prove is guest Mesa's encoder, and that is
 * fine: R4/R5 exercise the real Mesa over the real transport, which is the
 * target anyway, and no vtest port is thrown away to get there.
 *
 * The stream is the smallest one that is still evidence:
 *
 *   CREATE_OBJECT(SURFACE)      a render target view of a resource
 *   SET_FRAMEBUFFER_STATE       bind it
 *   CLEAR                       to a colour nothing else would produce
 *
 * then virgl_renderer_transfer_read_iov pulls the pixels back through the
 * renderer's own path. A checksum alone would not distinguish "rendered" from
 * "returned the buffer we passed in", so the clear colour is asserted per
 * channel and a second clear with a different colour must change the result --
 * a stubbed renderer passes the first test and fails the second.
 *
 * Build: see build.sh beside this file.
 */

#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>

#include <virgl/virglrenderer.h>
#include "wgl_winsys.h"

/*
 * Two structures the public header only forward-declares, because on Linux
 * they arrive from <sys/uio.h> and virgl_hw.h. Both are ABI, so they are
 * reproduced exactly rather than approximated: iovec as virglrenderer's own
 * non-Linux definition in src/vrend/vrend_iov.h, and virgl_box from
 * src/virgl_hw.h. Getting either wrong would be silent corruption rather than
 * a compile error, so they carry their source.
 */
struct iovec {
	void  *iov_base;
	size_t iov_len;
};

struct virgl_box {
	uint32_t x, y, z;
	uint32_t w, h, d;
};

/* From virglrenderer's src/virgl_protocol.h, which is not installed. These are
 * ABI: the guest's Mesa encodes exactly these numbers. */
#define VIRGL_CMD0(cmd, obj, len) ((cmd) | ((obj) << 8) | ((len) << 16))

/*
 * Counted out of enum virgl_context_cmd and enum virgl_object_type rather than
 * guessed. Guessing cost a round trip: opcode 4 is SET_VIEWPORT_STATE, not
 * SET_FRAMEBUFFER_STATE, and the decoder said so exactly ("failed to dispatch
 * SET_VIEWPORT_STATE") -- which is worth knowing, because it means vrend was
 * already decoding our stream and complaining about its content.
 */
#define VIRGL_CCMD_CREATE_OBJECT            1
#define VIRGL_CCMD_SET_FRAMEBUFFER_STATE    5
#define VIRGL_CCMD_CLEAR                    7

#define VIRGL_OBJECT_SURFACE                8

#define VIRGL_OBJ_SURFACE_SIZE              5

#define VIRGL_SET_FRAMEBUFFER_STATE_SIZE(n) (2 + (n))

/* PIPE_CLEAR_COLOR0 */
#define PIPE_CLEAR_COLOR0                   (1 << 2)

/* PIPE_FORMAT_B8G8R8A8_UNORM -- what the guest asks for a window surface. */
#define PIPE_FORMAT_B8G8R8A8_UNORM          1
#define PIPE_TEXTURE_2D                     2
#define PIPE_BIND_RENDER_TARGET             (1 << 1)
#define PIPE_BIND_SAMPLER_VIEW              (1 << 3)

#define W 256
#define H 256

#define RES_ID  1
#define CTX_ID  1
#define SURF_ID 10

static uint32_t cmd[256];
static int cmd_n;

static void emit(uint32_t v)
{
	if (cmd_n < (int)(sizeof(cmd) / sizeof(cmd[0])))
		cmd[cmd_n++] = v;
}

static void emit_f(float f)
{
	uint32_t u;

	memcpy(&u, &f, 4);
	emit(u);
}

/* virglrenderer calls this when a fence retires. The spike is synchronous, so
 * it only records; R4's daemon is where this wakes a sleeping guest thread. */
static uint32_t last_fence;

static void cb_write_fence(void *cookie, uint32_t fence)
{
	(void)cookie;
	last_fence = fence;
}

static struct virgl_renderer_callbacks cbs = {
	.version = 1,               /* v1: the three winsys callbacks only */
	.write_fence = cb_write_fence,
	.create_gl_context = wgl_create_context,
	.destroy_gl_context = wgl_destroy_context,
	.make_current = wgl_make_current,
};

/*
 * Build and submit: bind a surface over the resource, clear it to `rgba`.
 * Returns 0 on success.
 */
static int clear_to(float r, float g, float b, float a)
{
	cmd_n = 0;

	/* CREATE_OBJECT(SURFACE): a render-target view of resource RES_ID. */
	emit(VIRGL_CMD0(VIRGL_CCMD_CREATE_OBJECT, VIRGL_OBJECT_SURFACE,
			VIRGL_OBJ_SURFACE_SIZE));
	emit(SURF_ID);
	emit(RES_ID);
	emit(PIPE_FORMAT_B8G8R8A8_UNORM);
	emit(0);                     /* first layer / level */
	emit(0);                     /* last layer */

	/* SET_FRAMEBUFFER_STATE: one colour buffer, no depth. */
	emit(VIRGL_CMD0(VIRGL_CCMD_SET_FRAMEBUFFER_STATE, 0,
			VIRGL_SET_FRAMEBUFFER_STATE_SIZE(1)));
	emit(1);                     /* nr_cbufs */
	emit(0);                     /* zsurf handle: none */
	emit(SURF_ID);

	/* CLEAR: buffers mask, then colour as 4 floats, depth (double), stencil. */
	emit(VIRGL_CMD0(VIRGL_CCMD_CLEAR, 0, 8));
	emit(PIPE_CLEAR_COLOR0);
	emit_f(r); emit_f(g); emit_f(b); emit_f(a);
	emit(0); emit(0);            /* depth, as a double */
	emit(0);                     /* stencil */

	if (virgl_renderer_submit_cmd(cmd, CTX_ID, cmd_n) < 0) {
		printf("  submit_cmd failed\n");
		return -1;
	}
	return 0;
}

static int read_back(unsigned char *out)
{
	struct iovec iov;
	struct virgl_box box;

	iov.iov_base = out;
	iov.iov_len = (size_t)W * H * 4;

	memset(&box, 0, sizeof(box));
	box.w = W;
	box.h = H;
	box.d = 1;

	if (virgl_renderer_transfer_read_iov(RES_ID, CTX_ID, 0, 0, 0, &box, 0,
					     &iov, 1) < 0) {
		printf("  transfer_read_iov failed\n");
		return -1;
	}
	return 0;
}

static void describe(const char *what, const unsigned char *p)
{
	size_t mid = ((size_t)(H / 2) * W + (W / 2)) * 4;

	printf("    %s centre BGRA %02x %02x %02x %02x\n", what,
	       p[mid], p[mid + 1], p[mid + 2], p[mid + 3]);
}

int main(void)
{
	struct virgl_renderer_resource_create_args res;
	unsigned char *pix1, *pix2;
	int rc, ok = 1;

	printf("virglprobe -- R2, does virglrenderer render through WGL\n\n");

	if (wgl_winsys_init() != 0) {
		printf("FAIL: no usable WGL context\n");
		return 1;
	}
	printf("winsys renderer : %s\n", wgl_winsys_renderer());
	printf("winsys version  : %s\n", wgl_winsys_version());

	/*
	 * No USE_EGL, no USE_GLX: those tell virglrenderer to bring its own
	 * winsys, and the entire Windows port is that it does not have to.
	 * No THREAD_SYNC either -- fences are polled here, which is also what
	 * XP will need.
	 */
	/*
	 * The cookie must not be NULL. virglrenderer rejects the call with
	 * "invalid renderer vrend callbacks" on `!cookie || !cbs`, which reads
	 * like a complaint about the callbacks and is actually about the cookie
	 * -- it is handed back to every callback and the library refuses to run
	 * without one. Here it points at nothing useful; R4's daemon puts its
	 * own state behind it.
	 */
	rc = virgl_renderer_init(&cbs, 0, &cbs);
	if (rc != 0) {
		printf("FAIL: virgl_renderer_init returned %d\n", rc);
		return 1;
	}
	printf("virgl_renderer_init: ok\n\n");

	rc = virgl_renderer_context_create(CTX_ID, (uint32_t)strlen("probe"),
					   "probe");
	if (rc != 0) {
		printf("FAIL: context_create returned %d\n", rc);
		return 1;
	}

	memset(&res, 0, sizeof(res));
	res.handle = RES_ID;
	res.target = PIPE_TEXTURE_2D;
	res.format = PIPE_FORMAT_B8G8R8A8_UNORM;
	res.bind = PIPE_BIND_RENDER_TARGET | PIPE_BIND_SAMPLER_VIEW;
	res.width = W;
	res.height = H;
	res.depth = 1;
	res.array_size = 1;
	res.nr_samples = 0;
	res.flags = 0;

	rc = virgl_renderer_resource_create(&res, NULL, 0);
	if (rc != 0) {
		printf("FAIL: resource_create returned %d\n", rc);
		return 1;
	}
	virgl_renderer_ctx_attach_resource(CTX_ID, RES_ID);
	printf("resource %dx%d created and attached\n\n", W, H);

	pix1 = malloc((size_t)W * H * 4);
	pix2 = malloc((size_t)W * H * 4);
	if (!pix1 || !pix2)
		return 1;
	/* Poison, so "the renderer never wrote anything" is visibly different
	 * from "the renderer wrote black". */
	memset(pix1, 0xcd, (size_t)W * H * 4);
	memset(pix2, 0xcd, (size_t)W * H * 4);

	printf("clear 1: r=0.25 g=0.50 b=0.75 a=1.0\n");
	if (clear_to(0.25f, 0.50f, 0.75f, 1.0f) != 0 || read_back(pix1) != 0)
		return 1;
	describe("clear1", pix1);

	printf("clear 2: r=1.00 g=0.00 b=0.00 a=1.0\n");
	if (clear_to(1.00f, 0.00f, 0.00f, 1.0f) != 0 || read_back(pix2) != 0)
		return 1;
	describe("clear2", pix2);

	/*
	 * The two checks that matter, and why there are two. The first says the
	 * pixels are what the command asked for -- BGRA, so 0.25/0.50/0.75
	 * arrives as bf 80 40 in B,G,R order. The second says a different
	 * command produced a different answer, which is what distinguishes a
	 * renderer from a stub that hands back a plausible constant.
	 */
	{
		size_t mid = ((size_t)(H / 2) * W + (W / 2)) * 4;
		/*
		 * One LSB of tolerance, because float-to-unorm rounding at the
		 * half-way point is implementation-defined and 0.50 lands exactly
		 * there: 0.50 * 255 = 127.5, which this driver resolves to 0x7f
		 * and an exact test asserting 0x80 fails a perfectly correct
		 * renderer. 0.25 and 0.75 are not on a boundary and do come back
		 * exact. The band is one LSB, not a shrug -- a wrong channel or a
		 * swapped component is still caught.
		 */
		#define NEAR(got, want) ((int)(got) >= (int)(want) - 1 && \
					 (int)(got) <= (int)(want) + 1)
		int c1 = NEAR(pix1[mid], 0xbf) && NEAR(pix1[mid+1], 0x80) &&
			 NEAR(pix1[mid+2], 0x40) && NEAR(pix1[mid+3], 0xff);
		int c2 = NEAR(pix2[mid], 0x00) && NEAR(pix2[mid+1], 0x00) &&
			 NEAR(pix2[mid+2], 0xff) && NEAR(pix2[mid+3], 0xff);

		printf("\n  clear 1 pixel exact : %s\n", c1 ? "yes" : "NO");
		printf("  clear 2 pixel exact : %s\n", c2 ? "yes" : "NO");
		printf("  the two differ      : %s\n",
		       memcmp(pix1, pix2, (size_t)W * H * 4) ? "yes" : "NO");

		if (!c1 || !c2)
			ok = 0;
		if (!memcmp(pix1, pix2, (size_t)W * H * 4))
			ok = 0;
	}

	virgl_renderer_ctx_detach_resource(CTX_ID, RES_ID);
	virgl_renderer_resource_unref(RES_ID);
	virgl_renderer_context_destroy(CTX_ID);
	virgl_renderer_cleanup(NULL);
	wgl_winsys_fini();

	printf("\n%s\n", ok ? "PASS" : "FAIL");
	return ok ? 0 : 1;
}
