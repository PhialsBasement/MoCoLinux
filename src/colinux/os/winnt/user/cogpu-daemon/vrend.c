/*
 * virglrenderer behind the ring: where a guest command becomes real GL.
 *
 * R4 proved a request can cross and be answered. This is what makes the answer
 * mean something: instead of the daemon replying OK to everything, the command
 * stream is handed to virglrenderer, which decodes it and issues actual
 * OpenGL against the host's card through the WGL context R1 measured at 4.6
 * core.
 *
 * The division of labour is worth being precise about, because it is what the
 * whole design rests on:
 *
 *   - COMMAND streams are COPIED before being decoded. They are small (a few
 *     KB a frame) and the guest can rewrite the buffer while virglrenderer is
 *     parsing it -- a time-of-check/time-of-use the guest wins, because it
 *     runs on its own processor. QEMU copies for exactly this reason.
 *
 *   - RESOURCE data is NOT copied, ever. Vertex buffers, textures and pixel
 *     data are attached as iovecs pointing straight into the guest's pages
 *     through R3's mappings. That is megabytes a frame that never move, and it
 *     is the property a virtual machine cannot have.
 *
 * Every guest-supplied extent is validated against the mapping table before
 * the first byte is read. A resource whose backing does not lie wholly inside
 * guest RAM is refused, not clamped: a clamped extent silently renders the
 * wrong thing, while a refused one is a message.
 */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include <virgl/virglrenderer.h>

#include "vrend.h"
#include "wgl_winsys.h"

static int	 vrend_ready;
static uint64_t	 vrend_fences_written;
static cogpu_fence_fn vrend_fence_cb;
static void	*vrend_fence_ctx;

/*
 * virglrenderer telling us a fence has retired.
 *
 * It is called from inside virgl_renderer_submit_cmd or from
 * virgl_renderer_poll, on this thread, so there is no locking here -- the
 * daemon is single-threaded by design and the ring is serviced by the same
 * thread that renders.
 */
static void vrend_write_fence(void *cookie, uint32_t fence)
{
	(void)cookie;
	vrend_fences_written++;
	if (vrend_fence_cb)
		vrend_fence_cb(vrend_fence_ctx, fence);
}

static struct virgl_renderer_callbacks vrend_cbs = {
	.version	   = 1,
	.write_fence	   = vrend_write_fence,
	.create_gl_context = wgl_create_context,
	.destroy_gl_context = wgl_destroy_context,
	.make_current	   = wgl_make_current,
};

int cogpu_vrend_init(cogpu_fence_fn fence_cb, void *fence_ctx)
{
	int rc;

	vrend_fence_cb	= fence_cb;
	vrend_fence_ctx	= fence_ctx;

	if (wgl_winsys_init() != 0)
		return -1;

	/*
	 * No USE_EGL and no USE_GLX: those tell virglrenderer to bring up its
	 * own winsys, which on Windows there is none of. The three callbacks
	 * above are the alternative the library documents, and the cookie must
	 * be non-NULL -- virgl_renderer_init checks `!cookie || !cbs` and
	 * returns -1, which cost an hour the first time.
	 */
	rc = virgl_renderer_init(&vrend_cbs, 0, &vrend_cbs);
	if (rc != 0)
		return rc;

	vrend_ready = 1;
	return 0;
}

const char *cogpu_vrend_renderer(void)
{
	return wgl_winsys_renderer();
}

int cogpu_vrend_ready(void)
{
	return vrend_ready;
}

/*
 * The capsets the renderer actually has, asked rather than asserted.
 *
 * virgl_renderer_get_cap_set is the only honest source: a hand-written capset
 * would tell Mesa the host can do things this GL context cannot, and Mesa
 * would then emit commands that fail somewhere far from here.
 */
void cogpu_vrend_capset(uint32_t set, uint32_t *max_ver, uint32_t *max_size)
{
	*max_ver = 0;
	*max_size = 0;
	if (vrend_ready)
		virgl_renderer_get_cap_set(set, max_ver, max_size);
}

void cogpu_vrend_fill_caps(uint32_t set, uint32_t version, void *caps)
{
	if (vrend_ready)
		virgl_renderer_fill_caps(set, version, caps);
}

int cogpu_vrend_ctx_create(uint32_t ctx_id, const char *name, uint32_t namelen)
{
	if (!vrend_ready)
		return -1;
	return virgl_renderer_context_create(ctx_id, namelen, name);
}

void cogpu_vrend_ctx_destroy(uint32_t ctx_id)
{
	if (vrend_ready)
		virgl_renderer_context_destroy(ctx_id);
}

/*
 * A command stream, copied and then executed.
 *
 * The copy is the TOCTOU boundary described at the top of this file. It is
 * bounded because a command buffer is small; if a guest ever submits one
 * larger than this it is refused rather than truncated, since a truncated
 * command stream decodes into something the guest did not ask for.
 */
#define COGPU_MAX_CMD_BYTES (1u << 20)

int cogpu_vrend_submit(uint32_t ctx_id, const void *cmds, uint32_t bytes)
{
	static unsigned char *staging;
	static uint32_t	      staging_size;

	if (!vrend_ready)
		return -1;
	if (bytes == 0 || (bytes & 3))
		return -1;
	if (bytes > COGPU_MAX_CMD_BYTES)
		return -1;

	if (bytes > staging_size) {
		unsigned char *p = realloc(staging, bytes);

		if (!p)
			return -1;
		staging	     = p;
		staging_size = bytes;
	}

	memcpy(staging, cmds, bytes);
	return virgl_renderer_submit_cmd(staging, (int)ctx_id, bytes / 4);
}

int cogpu_vrend_resource_create(struct virgl_renderer_resource_create_args *args)
{
	if (!vrend_ready)
		return -1;
	return virgl_renderer_resource_create(args, NULL, 0);
}

void cogpu_vrend_resource_unref(uint32_t res_id)
{
	if (vrend_ready)
		virgl_renderer_resource_unref(res_id);
}

int cogpu_vrend_attach_iov(uint32_t res_id, struct iovec *iov, int niov)
{
	if (!vrend_ready)
		return -1;
	return virgl_renderer_resource_attach_iov(res_id, iov, niov);
}

void cogpu_vrend_detach_iov(uint32_t res_id)
{
	if (vrend_ready)
		virgl_renderer_resource_detach_iov(res_id, NULL, NULL);
}

void cogpu_vrend_ctx_attach(uint32_t ctx_id, uint32_t res_id)
{
	if (vrend_ready)
		virgl_renderer_ctx_attach_resource((int)ctx_id, (int)res_id);
}

void cogpu_vrend_poll(void)
{
	if (vrend_ready)
		virgl_renderer_poll();
}

uint64_t cogpu_vrend_fences(void)
{
	return vrend_fences_written;
}
