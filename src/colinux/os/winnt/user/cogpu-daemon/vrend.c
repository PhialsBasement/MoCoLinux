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

/*
 * Both are forward-declared in virglrenderer.h and defined in headers the
 * install does not ship (virgl_hw.h stays internal; sys/uio.h does not exist
 * on mingw). Six words and two, and virglrenderer only reads the members.
 */
struct virgl_box { uint32_t x, y, z, w, h, d; };
struct iovec { void *iov_base; size_t iov_len; };

#include <virgl/virglrenderer.h>

#include "vrend.h"
#include "wgl_winsys.h"

static int	 vrend_ready;
static uint64_t	 vrend_fences_written;
static cogpu_fence_fn vrend_fence_cb;
static void	*vrend_fence_ctx;
static cogpu_moco_present_fn vrend_moco_present_cb;

void cogpu_vrend_set_present_hook(cogpu_moco_present_fn fn)
{
	vrend_moco_present_cb = fn;
}

/*
 * virgl_renderer_resource_get_info() exposes the GL name and dimensions but
 * not the Gallium target, bind flags, or sample count. Those are precisely the
 * fields which distinguish a presentable 2D colour target from a buffer,
 * depth image, array, or multisample attachment. Keep the successful create
 * arguments beside the renderer object instead of guessing from a GL name.
 *
 * This list is touched only by the daemon's renderer thread. A missing entry
 * disables presentation for that resource; it must never make rendering fail.
 */
struct vrend_resource_meta {
	struct cogpu_vrend_resource_info info;
	struct vrend_resource_meta *next;
};

static struct vrend_resource_meta *vrend_resources;

static struct vrend_resource_meta *vrend_resource_find(uint32_t resource_id)
{
	struct vrend_resource_meta *meta;

	for (meta = vrend_resources; meta; meta = meta->next)
		if (meta->info.resource_id == resource_id)
			return meta;
	return NULL;
}

static void vrend_resource_remember(
			 const struct virgl_renderer_resource_create_args *args)
{
	struct vrend_resource_meta *meta = vrend_resource_find(args->handle);

	if (!meta) {
		meta = calloc(1, sizeof(*meta));
		if (!meta)
			return;
		meta->next = vrend_resources;
		vrend_resources = meta;
	}

	memset(&meta->info, 0, sizeof(meta->info));
	meta->info.resource_id = args->handle;
	meta->info.target = args->target;
	meta->info.format = args->format;
	meta->info.bind = args->bind;
	meta->info.width = args->width;
	meta->info.height = args->height;
	meta->info.depth = args->depth;
	meta->info.array_size = args->array_size;
	meta->info.last_level = args->last_level;
	meta->info.nr_samples = args->nr_samples;
	meta->info.flags = args->flags;
}

static void vrend_resource_forget(uint32_t resource_id)
{
	struct vrend_resource_meta **link = &vrend_resources;

	while (*link) {
		struct vrend_resource_meta *meta = *link;

		if (meta->info.resource_id == resource_id) {
			*link = meta->next;
			free(meta);
			return;
		}
		link = &meta->next;
	}
}

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

/*
 * The guest asked for a resource to be shown, through a command in its own
 * rendering stream rather than through a side channel. That is what makes
 * sandboxed clients work untouched: a container that can render can present,
 * because presenting IS rendering. Called on the renderer thread from inside
 * virgl_renderer_submit_cmd, with the resource already validated as owned by
 * the calling context.
 */
/*
 * Recorded here, performed after the command buffer finishes.
 *
 * Presenting inline corrupts the guest: this callback runs from inside
 * virgl_renderer_submit_cmd, part-way through a command buffer vrend is still
 * executing, and presenting makes a different WGL context current to draw the
 * overlay. vrend resumes the rest of the buffer against GL state it no longer
 * owns, and the guest's next CLEAR fails with GL_INVALID_FRAMEBUFFER_OPERATION
 * (1286) -- after which its context is poisoned and every DRAW_VBO is refused
 * for the life of the application. glxgears died exactly that way.
 *
 * The buffer's own commands therefore run to completion first, which is also
 * where the earlier ring-driven present ran, and is the ordering the presenter
 * was written against.
 */
static struct {
	int	 pending;
	uint32_t res_handle, xid, width, height;
	uint32_t damage_x, damage_y, damage_width, damage_height;
} vrend_present_request;

static void vrend_moco_present(void *cookie, uint32_t res_handle, uint32_t xid,
			       uint32_t width, uint32_t height,
			       uint32_t damage_x, uint32_t damage_y,
			       uint32_t damage_width, uint32_t damage_height)
{
	(void)cookie;
	/* Last request in a buffer wins: a frame presented twice in one submit
	 * has only its final contents on screen anyway. */
	vrend_present_request.pending	    = 1;
	vrend_present_request.res_handle    = res_handle;
	vrend_present_request.xid	    = xid;
	vrend_present_request.width	    = width;
	vrend_present_request.height	    = height;
	vrend_present_request.damage_x	    = damage_x;
	vrend_present_request.damage_y	    = damage_y;
	vrend_present_request.damage_width  = damage_width;
	vrend_present_request.damage_height = damage_height;
}

static void vrend_present_flush(void)
{
	if (!vrend_present_request.pending)
		return;
	vrend_present_request.pending = 0;
	if (vrend_moco_present_cb)
		vrend_moco_present_cb(vrend_present_request.res_handle,
				      vrend_present_request.xid,
				      vrend_present_request.width,
				      vrend_present_request.height,
				      vrend_present_request.damage_x,
				      vrend_present_request.damage_y,
				      vrend_present_request.damage_width,
				      vrend_present_request.damage_height);
}

/*
 * Venus fences retire on vkr's ring threads, NOT on the thread that
 * submitted -- that is the point of ASYNC_FENCE_CB. This forwards to the
 * daemon, which must treat it as a cross-thread signal and do nothing to the
 * vrings here.
 */
static cogpu_ctx_fence_fn vrend_ctx_fence_cb;

static void vrend_write_context_fence(void *cookie, uint32_t ctx_id,
				      uint32_t ring_idx, uint64_t fence_id)
{
	(void)cookie;
	if (vrend_ctx_fence_cb)
		vrend_ctx_fence_cb(ctx_id, ring_idx, fence_id);
}

void cogpu_vrend_set_ctx_fence_cb(cogpu_ctx_fence_fn fn)
{
	vrend_ctx_fence_cb = fn;
}

static int vrend_venus;

int cogpu_vrend_has_venus(void)
{
	return vrend_venus;
}

static struct virgl_renderer_callbacks vrend_cbs = {
	.version	   = 5,
	.write_fence	   = vrend_write_fence,
	.create_gl_context = wgl_create_context,
	.destroy_gl_context = wgl_destroy_context,
	.make_current	   = wgl_make_current,
	.write_context_fence = vrend_write_context_fence,
	.moco_present	   = vrend_moco_present,
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
	 *
	 * Venus first, GL alone as the fallback. The DLL does not import
	 * vulkan-1.dll -- it loads it at runtime -- so on a machine with no
	 * Vulkan the VENUS init fails cleanly and the retry without it is
	 * the machine's honest capability, not an error.
	 *
	 * RENDER_SERVER is not optional and not a separate process here:
	 * upstream removed in-process venus, so venus contexts exist only
	 * behind the proxy, and the WINQ port runs that "server" as
	 * in-process worker threads over localhost sockets. VENUS alone
	 * initialises nothing venus at all -- it only flips vrend's buffer
	 * layout, which is how an earlier build logged "venus: up" while
	 * serving a zeroed capset.
	 */
	rc = virgl_renderer_init(&vrend_cbs,
				 VIRGL_RENDERER_VENUS |
				 VIRGL_RENDERER_RENDER_SERVER |
				 VIRGL_RENDERER_ASYNC_FENCE_CB,
				 &vrend_cbs);
	if (rc == 0) {
		vrend_venus = 1;
	} else {
		rc = virgl_renderer_init(&vrend_cbs, 0, &vrend_cbs);
		if (rc != 0)
			return rc;
	}

	vrend_ready = 1;
	return 0;
}

/*
 * A per-context fence for a Venus ring. The completion arrives later on
 * vrend_write_context_fence; fences on one ring retire in submission order,
 * which is what lets the daemon publish every parked request up to the
 * signalled id.
 */
int cogpu_vrend_ctx_fence(uint32_t ctx_id, uint32_t ring_idx,
			  uint64_t fence_id)
{
	if (!vrend_ready)
		return -1;
	return virgl_renderer_context_create_fence(ctx_id, 0, ring_idx,
						   fence_id);
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

/*
 * A context, created for the capset the guest asked for.
 *
 * The capset id rides in the low byte of the context flags, and getting it
 * wrong is not a clean failure: the context is created, CTX_CREATE returns
 * success, and then every command stream is rejected because Mesa encodes for
 * VIRGL2 while the context was made for something else. What that looks like
 * from the guest is "response 0x1200 (command 0x207)" and a black framebuffer
 * -- a renderer that says virgl and renders nothing.
 *
 * virtio-gpu passes the guest's choice in the CTX_CREATE context_init field,
 * which carries the capset id in the same low byte.
 */
int cogpu_vrend_ctx_create(uint32_t ctx_id, const char *name, uint32_t namelen,
			   uint32_t context_init)
{
	uint32_t capset = context_init & VIRGL_RENDERER_CONTEXT_FLAG_CAPSET_ID_MASK;

	if (!vrend_ready)
		return -1;

	/* No capset named means the legacy default, which is VIRGL2 here --
	 * it is what this renderer advertises and what Mesa encodes for. */
	if (capset == 0)
		capset = 2;

	return virgl_renderer_context_create_with_flags(ctx_id, capset,
							namelen, name);
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

/* Command IDs and fields from virgl_protocol.h. That header is private to
 * virglrenderer and is intentionally absent from the installed SDK; these few
 * wire values are stable protocol, not renderer internals. */
#define COGPU_VIRGL_CCMD_TRANSFER3D                   43u
#define COGPU_VIRGL_CCMD_COPY_TRANSFER3D              45u
#define COGPU_VIRGL_TRANSFER3D_SIZE                   13u
#define COGPU_VIRGL_COPY_TRANSFER3D_SIZE              14u
#define COGPU_VIRGL_TRANSFER_FROM_HOST                 2u
#define COGPU_VIRGL_COPY_TRANSFER3D_READ_FROM_HOST    (1u << 1)

static void vrend_add_present_hint(struct cogpu_vrend_submit_info *info,
				   const uint32_t *command)
{
	struct cogpu_vrend_present_hint *hint;
	unsigned int i;

	if (!info)
		return;

	/* One submit can contain repeated readback packets for the same resource.
	 * Keep its last box, while preserving distinct candidates for the window
	 * and colour-target filters in present.c. */
	for (i = 0; i < info->present_hint_count; i++)
		if (info->present_hints[i].resource_id == command[1])
			break;
	if (i == COGPU_VREND_MAX_PRESENT_HINTS) {
		memmove(&info->present_hints[0], &info->present_hints[1],
			(COGPU_VREND_MAX_PRESENT_HINTS - 1) *
			sizeof(info->present_hints[0]));
		i--;
	} else if (i == info->present_hint_count) {
		info->present_hint_count++;
	}

	hint = &info->present_hints[i];
	hint->resource_id = command[1];
	hint->level = command[2];
	hint->x = command[6];
	hint->y = command[7];
	hint->width = command[9];
	hint->height = command[10];
}

static void vrend_find_present_hints(const uint32_t *stream, uint32_t dwords,
				     struct cogpu_vrend_submit_info *info)
{
	uint32_t offset = 0;

	if (!info)
		return;
	memset(info, 0, sizeof(*info));

	while (offset < dwords) {
		const uint32_t *command = &stream[offset];
		uint32_t length = command[0] >> 16;
		uint32_t opcode = command[0] & 0xff;

		if (length > dwords - offset - 1)
			return;
		if (opcode == COGPU_VIRGL_CCMD_TRANSFER3D &&
		    length >= COGPU_VIRGL_TRANSFER3D_SIZE &&
		    command[13] == COGPU_VIRGL_TRANSFER_FROM_HOST)
			vrend_add_present_hint(info, command);
		else if (opcode == COGPU_VIRGL_CCMD_COPY_TRANSFER3D &&
			 length == COGPU_VIRGL_COPY_TRANSFER3D_SIZE &&
			 (command[14] &
			  COGPU_VIRGL_COPY_TRANSFER3D_READ_FROM_HOST))
			/* For FROM_HOST, field 1 is the host texture source and
			 * field 12 is the guest-backed staging destination. */
			vrend_add_present_hint(info, command);

		offset += length + 1;
	}
}

int cogpu_vrend_submit(uint32_t ctx_id, const void *cmds, uint32_t bytes,
		       struct cogpu_vrend_submit_info *submit_info)
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
	vrend_find_present_hints((const uint32_t *)staging, bytes / 4,
				 submit_info);
	{
		int rc = virgl_renderer_submit_cmd(staging, (int)ctx_id,
						   bytes / 4);

		/* Only now, with the buffer's own commands finished and vrend
		 * no longer part-way through them. */
		vrend_present_flush();
		return rc;
	}
}

int cogpu_vrend_resource_create(struct virgl_renderer_resource_create_args *args)
{
	int rc;

	if (!vrend_ready)
		return -1;
	rc = virgl_renderer_resource_create(args, NULL, 0);
	if (rc == 0)
		vrend_resource_remember(args);
	return rc;
}

void cogpu_vrend_resource_unref(uint32_t res_id)
{
	if (vrend_ready) {
		virgl_renderer_resource_unref(res_id);
		vrend_resource_forget(res_id);
	}
}

int cogpu_vrend_resource_info(uint32_t res_id,
			      struct cogpu_vrend_resource_info *info)
{
	struct vrend_resource_meta *meta;
	struct virgl_renderer_resource_info renderer_info;
	int rc;

	if (!vrend_ready || !info)
		return -1;
	meta = vrend_resource_find(res_id);
	if (!meta)
		return -1;

	memset(&renderer_info, 0, sizeof(renderer_info));
	rc = virgl_renderer_resource_get_info((int)res_id, &renderer_info);
	if (rc != 0 || renderer_info.tex_id == 0)
		return -1;

	*info = meta->info;
	/* The renderer is authoritative after resource creation. */
	info->format = renderer_info.virgl_format;
	info->width = renderer_info.width;
	info->height = renderer_info.height;
	info->depth = renderer_info.depth;
	info->flags = renderer_info.flags;
	info->tex_id = renderer_info.tex_id;
	return 0;
}

/*
 * The iovec array belongs to the resource, and virglrenderer KEEPS THE POINTER.
 *
 * virgl_renderer_resource_attach_iov does not copy: virgl_resource.c and
 * vrend_renderer.c both simply store `res->iov = iov`. The array must
 * therefore live as long as the resource does, and be a separate allocation
 * per resource -- which is how QEMU uses this API, handing the pointer back at
 * detach time to be freed.
 *
 * Passing one shared buffer instead is catastrophic and quiet. Every resource
 * ends up pointing at the same memory, so each new attach silently rewrites
 * the backing of every resource attached before it. The visible half is
 * virglrenderer refusing transfers -- "IOV data size exceeds resource
 * capacity", each resource short by an arbitrary amount -- because it sums an
 * array that now describes somebody else's pages. The invisible half is worse:
 * when the overwritten entries happen to sum large enough to pass the bounds
 * check, the transfer reads and writes the WRONG GUEST PAGES, which is guest
 * memory corruption with no error anywhere.
 *
 * So the array is copied here, once per attach, and freed at detach.
 */
int cogpu_vrend_attach_iov(uint32_t res_id, struct iovec *iov, int niov)
{
	struct iovec *own;

	if (!vrend_ready || niov <= 0)
		return -1;

	own = malloc((size_t)niov * sizeof(*own));
	if (!own)
		return -1;
	memcpy(own, iov, (size_t)niov * sizeof(*own));

	if (virgl_renderer_resource_attach_iov(res_id, own, niov) != 0) {
		free(own);
		return -1;
	}
	return 0;
}

/*
 * Blob resources. The GUEST kind carries mem entries exactly like
 * ATTACH_BACKING, and the same ownership rule applies: virglrenderer keeps
 * the iovec pointer, so the caller passes an array this function copies.
 * HOST3D carries none.
 */
int cogpu_vrend_create_blob(uint32_t ctx_id, uint32_t res_id,
			    uint32_t blob_mem, uint32_t blob_flags,
			    uint64_t blob_id, uint64_t size,
			    struct iovec *iov, int niov)
{
	struct virgl_renderer_resource_create_blob_args args;
	struct iovec *own = NULL;
	int rc;

	if (!vrend_ready)
		return -1;

	if (niov > 0) {
		own = malloc((size_t)niov * sizeof(*own));
		if (!own)
			return -1;
		memcpy(own, iov, (size_t)niov * sizeof(*own));
	}

	memset(&args, 0, sizeof(args));
	args.res_handle = res_id;
	args.ctx_id	= ctx_id;
	args.blob_mem	= blob_mem;
	args.blob_flags = blob_flags;
	args.blob_id	= blob_id;
	args.size	= size;
	args.iovecs	= own;
	args.num_iovs	= (uint32_t)(niov > 0 ? niov : 0);

	rc = virgl_renderer_resource_create_blob(&args);
	if (rc != 0)
		free(own);
	return rc;
}

int cogpu_vrend_resource_map(uint32_t res_id, void **va, uint64_t *size)
{
	if (!vrend_ready)
		return -1;
	return virgl_renderer_resource_map(res_id, va, size);
}

int cogpu_vrend_resource_unmap(uint32_t res_id)
{
	if (!vrend_ready)
		return -1;
	return virgl_renderer_resource_unmap(res_id);
}

int cogpu_vrend_map_info(uint32_t res_id, uint32_t *map_info)
{
	if (!vrend_ready)
		return -1;
	return virgl_renderer_resource_get_map_info(res_id, map_info);
}

/*
 * Detach hands the array back so it can be freed. Passing NULL for both
 * out-parameters -- which this did -- discards the pointer and leaks one
 * allocation per resource for the life of the daemon.
 */
void cogpu_vrend_detach_iov(uint32_t res_id)
{
	struct iovec *iov = NULL;
	int niov = 0;

	if (!vrend_ready)
		return;

	virgl_renderer_resource_detach_iov(res_id, &iov, &niov);
	free(iov);
}

void cogpu_vrend_ctx_attach(uint32_t ctx_id, uint32_t res_id)
{
	if (vrend_ready)
		virgl_renderer_ctx_attach_resource((int)ctx_id, (int)res_id);
}

void cogpu_vrend_ctx_detach(uint32_t ctx_id, uint32_t res_id)
{
	if (vrend_ready)
		virgl_renderer_ctx_detach_resource((int)ctx_id, (int)res_id);
}

/*
 * A transfer between the guest's backing pages and the renderer's texture.
 *
 * The iovec is NULL on purpose: the resource already has its backing attached
 * (ATTACH_BACKING), so virglrenderer uses that, which is the guest's own pages
 * through R3's mappings. Passing a fresh iovec here would be the copy this
 * design exists to avoid.
 */
int cogpu_vrend_transfer(int to_host, uint32_t res_id, uint32_t ctx_id,
			 uint32_t level, uint32_t stride, uint32_t layer_stride,
			 uint32_t x, uint32_t y, uint32_t z,
			 uint32_t w, uint32_t h, uint32_t d, uint64_t offset)
{
	struct virgl_box box;

	if (!vrend_ready)
		return -1;

	box.x = x; box.y = y; box.z = z;
	box.w = w; box.h = h; box.d = d;

	if (to_host)
		return virgl_renderer_transfer_write_iov(res_id, ctx_id, (int)level,
							 stride, layer_stride,
							 &box, offset, NULL, 0);

	return virgl_renderer_transfer_read_iov(res_id, ctx_id, level,
						stride, layer_stride,
						&box, offset, NULL, 0);
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
