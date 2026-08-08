/*
 * cogpu-daemon -- the host side of the guest's GPU.
 *
 * A sibling of colinux-slirp-net-daemon.exe: an ordinary Windows program that
 * services one of the guest's device rings. It runs in userspace because that
 * is where WGL and the NVIDIA driver are, and it will eventually embed
 * virglrenderer. This is the transport half of it -- everything up to and
 * including a fenced request going round -- with the renderer stubbed.
 *
 * How it reaches the guest, and why that is unusual: there is no copy in or
 * out. The driver maps persistent 12 MB windows on first touch, so a descriptor
 * becomes a pointer once and stays one for the daemon's lifetime. Mapping the
 * whole guest at startup did the same job for 1 GB, but coupled --mem to the
 * number of Windows VADs and bugchecked at 2 GB; it is impossible at the
 * supported 128 GB ceiling. Working-set mappings keep the hot path arithmetic
 * while making untouched RAM free here.
 *
 * How it is told there is work: it is not told, it looks. The guest increments
 * a counter in shared memory and carries on; this process spin-polls that
 * counter on another core. There is no crossing, no interrupt and no ioctl in
 * the submission path at all -- which is the whole reason the design is worth
 * building, and why the poll is deliberately hot rather than a 20 ms sleep
 * like the older daemons.
 *
 *   cogpu-daemon.exe [--verbose] [--selftest] [--once]
 */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include <colinux/common/common.h>
#include <colinux/common/debug.h>
#include <colinux/user/manager.h>
#include <colinux/os/user/manager.h>
#include <colinux/os/alloc.h>

#include "vring.h"
#include "vrend.h"
/*
 * mingw has no sys/uio.h, and virglrenderer's header takes struct iovec as
 * given. Two members, same layout as everywhere else; virglrenderer only ever
 * reads iov_base and iov_len.
 */
struct iovec { void *iov_base; size_t iov_len; };
#include <virgl/virglrenderer.h>

COLINUX_DEFINE_MODULE("cogpu-daemon");

/* Must match src/colinux/kernel/vgpu.h and the guest's virtio_colinux.c. */
#define CO_VGPU_MAGIC		0x55504756
#define CO_VGPU_NUM_VQS		2

struct co_vgpu_vq_abi {
	unsigned long long desc_gpa, avail_gpa, used_gpa;
	unsigned int	   num, kick;
};

struct co_vgpu_io_abi {
	unsigned int	      magic, abi_version;
	unsigned long long    host_features, guest_features;
	unsigned int	      status, enabled, num_capsets, num_scanouts;
	struct co_vgpu_vq_abi vq[CO_VGPU_NUM_VQS];
	unsigned int	      used_pending, reaped;
	unsigned int	      guest_heartbeat, host_heartbeat;
};

/* virtio status bits, from the spec. */
#define VIRTIO_S_DRIVER_OK	4

static int verbose;

/*
 * Its own log file, written and flushed line by line.
 *
 * Every other daemon in this tree does this and it is not a style choice: a
 * long-running process redirected to a file never flushes, so the file stays
 * empty for as long as the daemon is useful and only fills in when it dies.
 * Three separate times this session that made a working daemon look hung.
 * fflush after every line costs nothing at these rates and means the log is
 * true at the instant it is read.
 */
static FILE *g_log;

/* The X wire (xwire.c) shares this daemon's log and guest-RAM mapping cache. */
int cogpu_xwire_start(void);

/*
 * The completion doorbell, shared with the X wire.
 *
 * co_manager_vgpu_wake cuts the monitor's idle sleep short so the guest
 * re-enters promptly instead of waiting out its backoff. It was built for GPU
 * completions, but the effect is generic -- "there is work, re-enter now" --
 * and the X wire needs exactly that: when a reply or an input event lands in
 * the guest's RX ring while the guest is idle waiting for it, nothing else
 * wakes it, so an interactive app pays a whole backoff tick per event and
 * feels like single-digit fps while throughput is fine. xwire.c rings this
 * from fill_rx. The handle is the daemon's, captured once at startup.
 */
static co_manager_handle_t g_wake_handle;

void cogpu_ring_doorbell(void)
{
	if (g_wake_handle)
		co_manager_vgpu_wake(g_wake_handle);
}

void logline(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	fflush(stdout);

	if (g_log) {
		va_start(ap, fmt);
		vfprintf(g_log, fmt, ap);
		va_end(ap);
		fflush(g_log);
	}
}

/* ------------------------------------------------------- the guest's memory */

struct mapped_slice {
	co_kmap_range_t	      range;
	struct mapped_slice *next;
};

/*
 * Immutable after publication. Hits take no lock; a miss is serialized so the
 * GPU thread and an X-wire thread cannot ask the driver for the same slice at
 * once. Nodes are never removed while the daemon is live because virglrenderer
 * retains iovec pointers until RESOURCE_DETACH_BACKING. KUNMAP at shutdown
 * retires the driver side in one operation.
 */
static struct mapped_slice *volatile g_slices;
static struct mapped_slice *volatile g_last_hit;
static CRITICAL_SECTION		  g_slice_lock;
static int			  g_slice_lock_ready;
static volatile LONG		  g_slice_count;
static unsigned long long	  g_slice_bytes;

static struct mapped_slice *slice_head(void)
{
	return (struct mapped_slice *)InterlockedCompareExchangePointer(
		(PVOID volatile *)&g_slices, NULL, NULL);
}

static struct mapped_slice *find_slice(uint64_t gpa, uint32_t len)
{
	struct mapped_slice *slice;
	uint64_t end = gpa + len;

	if (end < gpa)
		return NULL;

	slice = (struct mapped_slice *)InterlockedCompareExchangePointer(
		(PVOID volatile *)&g_last_hit, NULL, NULL);
	if (slice && gpa >= slice->range.pa &&
	    end <= slice->range.pa + slice->range.bytes)
		return slice;

	for (slice = slice_head(); slice; slice = slice->next) {
		if (gpa >= slice->range.pa &&
		    end <= slice->range.pa + slice->range.bytes) {
			InterlockedExchangePointer((PVOID volatile *)&g_last_hit, slice);
			return slice;
		}
	}
	return NULL;
}

static struct mapped_slice *map_slice(uint64_t gpa)
{
	struct mapped_slice *slice;
	co_kmap_range_t range;
	MEMORY_BASIC_INFORMATION mbi;
	int reused = 0;

	slice = find_slice(gpa, 1);
	if (slice || !g_wake_handle || !g_slice_lock_ready)
		return slice;

	EnterCriticalSection(&g_slice_lock);
	slice = find_slice(gpa, 1);
	if (slice)
		goto out;

	slice = calloc(1, sizeof(*slice));
	if (!slice)
		goto out;
	if (!CO_OK(co_manager_kmap_range(g_wake_handle, gpa, &range, &reused))) {
		free(slice);
		slice = NULL;
		goto out;
	}

	/* Refuse a mismatched driver/daemon ABI before following its pointer. */
	if (range.bytes == 0 || range.pa + range.bytes < range.pa ||
	    gpa < range.pa || gpa >= range.pa + range.bytes) {
		logline("KMAP_RANGE returned invalid range for 0x%llx:"
			" pa 0x%llx bytes 0x%llx\n",
			(unsigned long long)gpa, range.pa, range.bytes);
		free(slice);
		slice = NULL;
		goto out;
	}

	ZeroMemory(&mbi, sizeof(mbi));
	if (VirtualQuery((void *)(size_t)range.user_va, &mbi, sizeof(mbi)) !=
			sizeof(mbi) ||
	    mbi.State != MEM_COMMIT ||
	    (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) ||
	    range.user_va + range.bytes < range.user_va ||
	    (unsigned long long)(size_t)mbi.BaseAddress > range.user_va ||
	    (unsigned long long)(size_t)mbi.BaseAddress + mbi.RegionSize <
			(unsigned long long)(size_t)mbi.BaseAddress ||
	    range.user_va + range.bytes >
			(unsigned long long)(size_t)mbi.BaseAddress + mbi.RegionSize) {
		logline("KMAP_RANGE pa 0x%llx returned unreadable user address"
			" 0x%llx (state 0x%lx protect 0x%lx)\n",
			range.pa, range.user_va, (unsigned long)mbi.State,
			(unsigned long)mbi.Protect);
		free(slice);
		slice = NULL;
		goto out;
	}

	slice->range = range;
	slice->next = slice_head();
	InterlockedExchangePointer((PVOID volatile *)&g_slices, slice);
	InterlockedExchangePointer((PVOID volatile *)&g_last_hit, slice);
	InterlockedIncrement(&g_slice_count);
	g_slice_bytes += range.bytes;
	logline("mapped working-set slice %ld: pa 0x%llx, %llu KB at"
		" 0x%llx%s\n", (long)g_slice_count, range.pa,
		range.bytes >> 10, range.user_va, reused ? " (driver reused)" : "");

out:
	LeaveCriticalSection(&g_slice_lock);
	return slice;
}

/*
 * How much of a buffer starting at `gpa` is reachable as one host pointer.
 *
 * Guest RAM is mapped in 12 MB slices, and those slices are separate MDL
 * mappings -- adjacent in guest-physical space, unrelated in host-virtual
 * space. A guest buffer crossing a boundary therefore has no single host
 * pointer, and resolving it as one made every large ATTACH_BACKING fail:
 * anything bigger than a slice, or unluckily placed, came back ERR_UNSPEC and
 * every draw against it failed afterwards. R5's probe buffers fitted inside
 * one slice by luck, which is why this survived that long.
 *
 * Returning the reachable prefix lets the caller split the entry into as many
 * iovecs as it spans -- which is what an iovec list is for.
 */
static uint32_t resolve_run(uint64_t gpa, uint32_t len)
{
	struct mapped_slice *slice = find_slice(gpa, 1);
	uint64_t avail;

	if (!slice)
		slice = map_slice(gpa);
	if (!slice)
		return 0;
	avail = slice->range.pa + slice->range.bytes - gpa;
	return (avail < len) ? (uint32_t)avail : len;
}

void *resolve_gpa(void *ctx, uint64_t gpa, uint32_t len)
{
	struct mapped_slice *slice;

	(void)ctx;
	slice = find_slice(gpa, len);
	if (!slice) {
		slice = map_slice(gpa);
		if (!slice || gpa + len < gpa ||
		    gpa + len > slice->range.pa + slice->range.bytes)
			return NULL;
	}
	return (void *)(size_t)(slice->range.user_va +
				       (gpa - slice->range.pa));
}

/*
 * Gather a payload that spans several readable descriptors.
 *
 * A virtio request is a CHAIN, and the guest splits its payload across as many
 * descriptors as its scatter list needed -- one per physically contiguous run.
 * Anything larger than a page therefore arrives in pieces, and Firefox submits
 * command streams over a hundred kilobytes long, which is dozens of them.
 *
 * Reading only in[1] and trusting its length is wrong twice over: a stream
 * that spans in[1]..in[N] gets truncated to its first fragment, and the
 * fragment boundary falls in the middle of a virgl command. What reaches
 * virglrenderer is a prefix ending mid-instruction, so the CREATE_OBJECT
 * commands near the end are simply absent while the binds that reference them
 * still execute -- which it reports as "Illegal handle" on a bind, a long way
 * from the truncation that caused it.
 *
 * Copying is unavoidable here and cheap at these sizes: the pieces are not
 * adjacent in this process's address space, and virglrenderer wants one
 * pointer. Command streams are copied before decoding anyway (see vrend.c on
 * the time-of-check/time-of-use boundary), so this replaces that copy rather
 * than adding one.
 */
static void *chain_gather(struct cogpu_chain *chain, int first,
			  uint32_t skip, uint32_t want, uint32_t *got)
{
	static unsigned char *buf;
	static uint32_t buf_size;
	uint32_t done = 0;
	int i;

	*got = 0;
	if (want == 0)
		return NULL;

	if (want > buf_size) {
		unsigned char *p = realloc(buf, want);

		if (!p)
			return NULL;
		buf	 = p;
		buf_size = want;
	}

	for (i = first; i < chain->in_count && done < want; i++) {
		const unsigned char *src = chain->in[i].addr;
		uint32_t len = chain->in[i].len;

		/* The header sits at the front of the first descriptor. */
		if (i == first) {
			if (len <= skip)
				continue;
			src += skip;
			len -= skip;
		}
		if (len > want - done)
			len = want - done;

		memcpy(buf + done, src, len);
		done += len;
	}

	*got = done;
	return buf;
}

/* -------------------------------------------------------- the virtio-gpu bits */

/*
 * Just enough of the protocol to answer what the driver asks during probe and
 * to complete a fenced no-op. The renderer is not here yet; what is being
 * proved is that a request crosses, is understood, and its fence comes back.
 */
/*
 * Counted off enum virtio_gpu_ctrl_type in the guest's own
 * include/uapi/linux/virtio_gpu.h rather than remembered. The first version of
 * this file guessed, and put GET_CAPSET_INFO at 0x0102 -- which is really
 * RESOURCE_UNREF -- so the capset query fell through to the default case, got
 * a bare OK, and the driver sat waiting five seconds for an answer it had
 * already been given. "timed out waiting for cap set 0" was that.
 */
#define VIRTIO_GPU_CMD_GET_DISPLAY_INFO		0x0100
#define VIRTIO_GPU_CMD_RESOURCE_CREATE_2D	0x0101
#define VIRTIO_GPU_CMD_RESOURCE_UNREF		0x0102
#define VIRTIO_GPU_CMD_SET_SCANOUT		0x0103
#define VIRTIO_GPU_CMD_RESOURCE_FLUSH		0x0104
#define VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D	0x0105
#define VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING	0x0106
#define VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING	0x0107
#define VIRTIO_GPU_CMD_GET_CAPSET_INFO		0x0108
#define VIRTIO_GPU_CMD_GET_CAPSET		0x0109
#define VIRTIO_GPU_CMD_GET_EDID			0x010a
#define VIRTIO_GPU_CMD_RESOURCE_ASSIGN_UUID	0x010b
#define VIRTIO_GPU_CMD_RESOURCE_CREATE_BLOB	0x010c

#define VIRTIO_GPU_CMD_CTX_CREATE		0x0200
#define VIRTIO_GPU_CMD_CTX_DESTROY		0x0201
#define VIRTIO_GPU_CMD_CTX_ATTACH_RESOURCE	0x0202
#define VIRTIO_GPU_CMD_CTX_DETACH_RESOURCE	0x0203
#define VIRTIO_GPU_CMD_RESOURCE_CREATE_3D	0x0204
#define VIRTIO_GPU_CMD_TRANSFER_TO_HOST_3D	0x0205
#define VIRTIO_GPU_CMD_TRANSFER_FROM_HOST_3D	0x0206
#define VIRTIO_GPU_CMD_SUBMIT_3D		0x0207

#define VIRTIO_GPU_RESP_OK_NODATA		0x1100
#define VIRTIO_GPU_RESP_OK_DISPLAY_INFO		0x1101
#define VIRTIO_GPU_RESP_OK_CAPSET_INFO		0x1102
#define VIRTIO_GPU_RESP_OK_CAPSET		0x1103
#define VIRTIO_GPU_RESP_ERR_UNSPEC		0x1200

#define VIRTIO_GPU_FLAG_FENCE			(1 << 0)

struct virtio_gpu_ctrl_hdr {
	uint32_t type;
	uint32_t flags;
	uint64_t fence_id;
	uint32_t ctx_id;
	uint8_t	 ring_idx;
	uint8_t	 padding[3];
};

struct gpu_stats {
	unsigned long long requests;
	unsigned long long fenced;
	unsigned long long refused;
	unsigned long long submits;
	unsigned long long cmd_bytes;
	unsigned long long resources;
	unsigned long long transfers;
	unsigned long long backing_bytes;
	unsigned long long by_type[16];
};

static struct gpu_stats g_stats;

/*
 * Answer one request.
 *
 * Returns the number of bytes written into the chain's writable half. Every
 * response begins with a control header, and when the request carried a fence
 * the response must echo its id -- that echo is what retires the guest's
 * dma_fence and wakes whatever slept on it. Getting it wrong does not produce
 * an error; it produces a thread that never wakes, which is why the fence id
 * is copied rather than recomputed.
 */
static uint32_t serve(struct cogpu_chain *chain)
{
	struct virtio_gpu_ctrl_hdr req, *resp;
	uint32_t type;
	/*
	 * How many bytes the reply actually occupies. This is published in the
	 * used ring and it is not decoration: a response carrying a payload
	 * that claims to be only a header long is a short reply, and the driver
	 * waits for the rest of an answer that has already been given. That is
	 * what "timed out waiting for cap set 0" was.
	 */
	uint32_t written;

	if (chain->in_count == 0 || chain->in[0].len < sizeof(req) ||
	    chain->out_count == 0 || chain->out[0].len < sizeof(*resp)) {
		g_stats.refused++;
		return 0;
	}

	memcpy(&req, chain->in[0].addr, sizeof(req));
	type = req.type;

	resp = (struct virtio_gpu_ctrl_hdr *)chain->out[0].addr;
	memset(resp, 0, sizeof(*resp));
	written = sizeof(*resp);

	switch (type) {
	case VIRTIO_GPU_CMD_GET_DISPLAY_INFO:
		/*
		 * No scanouts, so the reply is a header and a block of zeroed
		 * display modes -- which is exactly "no displays", and what a
		 * render-only device should say.
		 */
		resp->type = VIRTIO_GPU_RESP_OK_DISPLAY_INFO;
		if (chain->out[0].len > sizeof(*resp)) {
			memset((char *)resp + sizeof(*resp), 0,
			       chain->out[0].len - sizeof(*resp));
			written = chain->out[0].len;
		}
		break;

	case VIRTIO_GPU_CMD_GET_CAPSET_INFO: {
		/*
		 * What the renderer can actually do, asked of virglrenderer
		 * rather than asserted. A hand-written capset tells Mesa the
		 * host supports things this GL context does not, and Mesa then
		 * emits commands that fail a long way from here.
		 *
		 * The guest asks by INDEX and we answer with an ID, and the two
		 * are not the same thing. Mesa's virgl driver wants
		 * VIRTIO_GPU_CAPSET_VIRGL2 (id 2) -- offering only id 1 gets
		 * "No virgl contexts available on host" and a silent fall back
		 * to llvmpipe, which looks exactly like the device not working.
		 */
		struct { uint32_t capset_index, pad; } q;
		struct { uint32_t capset_id, version, size, pad; } *info =
			(void *)((char *)resp + sizeof(*resp));

		if (chain->out[0].len < sizeof(*resp) + sizeof(*info)) {
			resp->type = VIRTIO_GPU_RESP_ERR_UNSPEC;
			break;
		}

		memset(&q, 0, sizeof(q));
		if (chain->in[0].len >= sizeof(req) + sizeof(q))
			memcpy(&q, (char *)chain->in[0].addr + sizeof(req), sizeof(q));

		/* index 0 -> VIRGL (1), index 1 -> VIRGL2 (2) */
		info->capset_id = (q.capset_index == 0) ? 1 : 2;
		cogpu_vrend_capset(info->capset_id, &info->version, &info->size);
		info->pad = 0;

		if (verbose)
			logline("  capset index %u -> id %u, version %u, %u bytes\n",
				q.capset_index, info->capset_id,
				info->version, info->size);

		resp->type = VIRTIO_GPU_RESP_OK_CAPSET_INFO;
		written	   = sizeof(*resp) + sizeof(*info);
		break;
	}

	case VIRTIO_GPU_CMD_GET_CAPSET: {
		struct { uint32_t capset_id, capset_version; } req2;

		if (chain->in[0].len < sizeof(req) + sizeof(req2)) {
			resp->type = VIRTIO_GPU_RESP_ERR_UNSPEC;
			break;
		}
		memcpy(&req2, (char *)chain->in[0].addr + sizeof(req), sizeof(req2));
		if (chain->out_count > 0 && chain->out[0].len > sizeof(*resp)) {
			cogpu_vrend_fill_caps(req2.capset_id, req2.capset_version,
					      (char *)resp + sizeof(*resp));
			written = chain->out[0].len;
		}
		resp->type = VIRTIO_GPU_RESP_OK_CAPSET;
		break;
	}

	case VIRTIO_GPU_CMD_CTX_CREATE: {
		struct { uint32_t namelen, ctx_init; char name[64]; } c;

		memset(&c, 0, sizeof(c));
		if (chain->in[0].len >= sizeof(req) + 8)
			memcpy(&c, (char *)chain->in[0].addr + sizeof(req),
			       (chain->in[0].len - sizeof(req)) < sizeof(c)
			       ? chain->in[0].len - sizeof(req) : sizeof(c));

		if (cogpu_vrend_ctx_create(req.ctx_id, c.name,
					   c.namelen > 63 ? 63 : c.namelen,
					   c.ctx_init) == 0) {
			resp->type = VIRTIO_GPU_RESP_OK_NODATA;
			if (verbose)
				logline("  ctx %u created, capset %u\n",
					req.ctx_id, c.ctx_init & 0xff);
		} else {
			resp->type = VIRTIO_GPU_RESP_ERR_UNSPEC;
		}
		break;
	}

	case VIRTIO_GPU_CMD_CTX_DESTROY:
		cogpu_vrend_ctx_destroy(req.ctx_id);
		resp->type = VIRTIO_GPU_RESP_OK_NODATA;
		break;

	case VIRTIO_GPU_CMD_SUBMIT_3D: {
		/*
		 * The command stream itself. Everything after the request
		 * header in the readable half is virgl commands; hand it to
		 * the renderer, which copies it before decoding (see vrend.c
		 * for why that copy is not optional).
		 */
		struct { uint32_t size, num_in_fences; } sub;
		const void *cmds;
		uint32_t avail;

		if (chain->in[0].len < sizeof(req) + sizeof(sub)) {
			resp->type = VIRTIO_GPU_RESP_ERR_UNSPEC;
			break;
		}
		memcpy(&sub, (char *)chain->in[0].addr + sizeof(req), sizeof(sub));

		/*
		 * The stream, gathered from however many descriptors it took.
		 * Firefox submits buffers of a hundred kilobytes and more --
		 * dozens of fragments -- and taking only the first one hands
		 * the decoder a prefix that ends mid-command.
		 */
		cmds = chain_gather(chain, 0, sizeof(req) + sizeof(sub),
				    sub.size, &avail);
		if (!cmds || avail < sub.size) {
			logline("  SUBMIT_3D: stream is %u bytes but the chain"
				" carries %u across %d descriptors\n",
				sub.size, avail, chain->in_count);
			resp->type = VIRTIO_GPU_RESP_ERR_UNSPEC;
			break;
		}

		{
			int rc2 = cogpu_vrend_submit(req.ctx_id, cmds, sub.size);

			if (rc2 == 0) {
				resp->type = VIRTIO_GPU_RESP_OK_NODATA;
				g_stats.submits++;
				g_stats.cmd_bytes += sub.size;
			} else {
				resp->type = VIRTIO_GPU_RESP_ERR_UNSPEC;
				logline("  SUBMIT_3D ctx %u, %u bytes, refused (%d)\n",
					req.ctx_id, sub.size, rc2);
			}
		}
		break;
	}

	case VIRTIO_GPU_CMD_RESOURCE_CREATE_3D: {
		/*
		 * A real resource in the renderer. This used to fall through to
		 * the default case and answer OK without creating anything --
		 * and the consequence was not a missing texture but a rejected
		 * command stream: SUBMIT_3D referencing a resource that does
		 * not exist comes back EINVAL, which reads as "the renderer is
		 * broken" and is really "you told the guest yes and did
		 * nothing".
		 */
		struct {
			uint32_t resource_id, target, format, bind;
			uint32_t width, height, depth, array_size;
			uint32_t last_level, nr_samples, flags, pad;
		} r;
		struct virgl_renderer_resource_create_args args;

		if (chain->in[0].len < sizeof(req) + sizeof(r)) {
			resp->type = VIRTIO_GPU_RESP_ERR_UNSPEC;
			break;
		}
		memcpy(&r, (char *)chain->in[0].addr + sizeof(req), sizeof(r));

		memset(&args, 0, sizeof(args));
		args.handle	= r.resource_id;
		args.target	= r.target;
		args.format	= r.format;
		args.bind	= r.bind;
		args.width	= r.width;
		args.height	= r.height;
		args.depth	= r.depth;
		args.array_size	= r.array_size;
		args.last_level	= r.last_level;
		args.nr_samples	= r.nr_samples;
		args.flags	= r.flags;

		if (cogpu_vrend_resource_create(&args) == 0) {
			resp->type = VIRTIO_GPU_RESP_OK_NODATA;
			g_stats.resources++;
		} else {
			resp->type = VIRTIO_GPU_RESP_ERR_UNSPEC;
			logline("  RESOURCE_CREATE_3D %u refused\n", r.resource_id);
		}
		break;
	}

	case VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING: {
		/*
		 * Where the zero copy actually happens.
		 *
		 * The guest hands a list of {guest physical address, length}
		 * and the host attaches them as iovecs pointing straight into
		 * those pages through R3's mappings. Nothing is copied: a
		 * texture the guest uploads is memory the renderer reads in
		 * place. Every extent is checked to lie wholly inside guest RAM
		 * before the first byte -- an extent that does not is refused,
		 * never clamped, because a clamped extent renders the wrong
		 * thing quietly.
		 */
		struct { uint32_t resource_id, nr_entries; } a;
		struct { uint64_t addr; uint32_t length, pad; } ent;
		/*
		 * Staging only. cogpu_vrend_attach_iov copies this into a
		 * per-resource allocation, because virglrenderer keeps the
		 * pointer it is given -- see vrend.c. Handing this array
		 * straight to it made every resource share one backing
		 * description.
		 */
		static struct iovec iov[4096];
		const char *p;
		uint32_t i;
		int bad = 0, niov = 0;

		if (chain->in[0].len < sizeof(req) + sizeof(a)) {
			resp->type = VIRTIO_GPU_RESP_ERR_UNSPEC;
			break;
		}
		memcpy(&a, (char *)chain->in[0].addr + sizeof(req), sizeof(a));

		if (a.nr_entries == 0 || a.nr_entries > 2048) {
			resp->type = VIRTIO_GPU_RESP_ERR_UNSPEC;
			break;
		}

		/*
		 * The entry list, gathered the same way. 256 entries is
		 * exactly one page, so a larger backing -- or a guest whose
		 * allocation fell back to vmalloc -- splits it across
		 * descriptors, and reading only the first would produce
		 * plausible garbage addresses rather than an error.
		 */
		{
			uint32_t want = a.nr_entries * (uint32_t)sizeof(ent);
			uint32_t got = 0;

			p = chain_gather(chain, 0, sizeof(req) + sizeof(a),
					 want, &got);
			if (!p || got < want) {
				logline("  ATTACH_BACKING res %u: %u entries need"
					" %u bytes, chain carries %u\n",
					a.resource_id, a.nr_entries, want, got);
				resp->type = VIRTIO_GPU_RESP_ERR_UNSPEC;
				break;
			}
		}

		niov = 0;
		for (i = 0; i < a.nr_entries && !bad; i++) {
			uint64_t addr;
			uint32_t left;

			memcpy(&ent, p + i * sizeof(ent), sizeof(ent));
			addr = ent.addr;
			left = ent.length;

			/*
			 * One entry can span several mapping slices, so it
			 * becomes several iovecs. Not an optimisation: a
			 * buffer crossing a boundary has no single host
			 * pointer, so splitting is the only correct answer.
			 */
			while (left && !bad) {
				uint32_t run = resolve_run(addr, left);
				void *host;

				if (run == 0) {
					logline("  ATTACH_BACKING res %u: entry %u"
						" (0x%llx +%u) outside guest RAM"
						" -- %ld working-set slices, %llu MB mapped\n",
						a.resource_id, i,
						(unsigned long long)addr, left,
						(long)g_slice_count,
						(unsigned long long)(g_slice_bytes >> 20));
					bad = 1;
					break;
				}
				if (niov >= (int)(sizeof(iov) / sizeof(iov[0]))) {
					logline("  ATTACH_BACKING res %u: over %d"
						" iovecs\n", a.resource_id,
						(int)(sizeof(iov) / sizeof(iov[0])));
					bad = 1;
					break;
				}

				host = resolve_gpa(NULL, addr, run);
				if (!host) {
					bad = 1;
					break;
				}

				iov[niov].iov_base = host;
				iov[niov].iov_len  = run;
				niov++;

				g_stats.backing_bytes += run;
				addr += run;
				left -= run;
			}
		}

		if (bad) {
			resp->type = VIRTIO_GPU_RESP_ERR_UNSPEC;
		} else if (cogpu_vrend_attach_iov(a.resource_id, iov, niov) != 0) {
			logline("  ATTACH_BACKING res %u: renderer refused"
				" %d iovecs\n", a.resource_id, niov);
			resp->type = VIRTIO_GPU_RESP_ERR_UNSPEC;
		} else {
			uint64_t total = 0;
			int k;

			for (k = 0; k < niov; k++)
				total += iov[k].iov_len;

			/*
			 * The attached size, logged on success too:
			 * virglrenderer refuses a transfer whose computed size
			 * exceeds the backing, and without this number the only
			 * symptom is EINVAL on a transfer whose own parameters
			 * look perfectly correct.
			 */
			logline("  ATTACH_BACKING res %u: %d iovecs,"
				" %llu bytes\n", a.resource_id, niov,
				(unsigned long long)total);
			resp->type = VIRTIO_GPU_RESP_OK_NODATA;
		}
		break;
	}

	case VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING: {
		struct { uint32_t resource_id, pad; } d;

		if (chain->in[0].len >= sizeof(req) + sizeof(d)) {
			memcpy(&d, (char *)chain->in[0].addr + sizeof(req), sizeof(d));
			cogpu_vrend_detach_iov(d.resource_id);
		}
		resp->type = VIRTIO_GPU_RESP_OK_NODATA;
		break;
	}

	case VIRTIO_GPU_CMD_RESOURCE_UNREF: {
		struct { uint32_t resource_id, pad; } d;

		if (chain->in[0].len < sizeof(req) + sizeof(d)) {
			logline("  RESOURCE_UNREF: short descriptor;"
				" NOT dropping the backing\n");
			resp->type = VIRTIO_GPU_RESP_ERR_UNSPEC;
			break;
		}
		memcpy(&d, (char *)chain->in[0].addr + sizeof(req), sizeof(d));

		/*
		 * Stop pointing at the guest's pages BEFORE acknowledging.
		 *
		 * virgl_renderer_resource_unref decrements a refcount. A
		 * resource still attached to a context -- and the guest is not
		 * obliged to send CTX_DETACH_RESOURCE first -- survives the
		 * unref with its iovecs intact, still aimed at guest physical
		 * pages. The guest driver frees those pages as soon as this
		 * reply lands, the allocator hands them to something else, and
		 * the next readback writes host pixels over whatever now lives
		 * there.
		 *
		 * Not a theory: it panicked the guest twice, once in
		 * landlock's cred-free hook reading a domain pointer of
		 * all-ones out of a recycled cred, and once as "Oops: Bad
		 * pagetable" in Xorg with the page table itself overwritten.
		 * Both are pages that had just been freed and reused.
		 */
		cogpu_vrend_detach_iov(d.resource_id);
		cogpu_vrend_resource_unref(d.resource_id);

		resp->type = VIRTIO_GPU_RESP_OK_NODATA;
		break;
	}

	case VIRTIO_GPU_CMD_CTX_ATTACH_RESOURCE: {
		struct { uint32_t resource_id, pad; } d;

		if (chain->in[0].len >= sizeof(req) + sizeof(d)) {
			memcpy(&d, (char *)chain->in[0].addr + sizeof(req), sizeof(d));
			cogpu_vrend_ctx_attach(req.ctx_id, d.resource_id);
		}
		resp->type = VIRTIO_GPU_RESP_OK_NODATA;
		break;
	}

	case VIRTIO_GPU_CMD_CTX_DETACH_RESOURCE: {
		struct { uint32_t resource_id, pad; } d;

		if (chain->in[0].len >= sizeof(req) + sizeof(d)) {
			memcpy(&d, (char *)chain->in[0].addr + sizeof(req), sizeof(d));
			cogpu_vrend_ctx_detach(req.ctx_id, d.resource_id);
		}
		resp->type = VIRTIO_GPU_RESP_OK_NODATA;
		break;
	}

	case VIRTIO_GPU_CMD_TRANSFER_TO_HOST_3D:
	case VIRTIO_GPU_CMD_TRANSFER_FROM_HOST_3D: {
		struct {
			uint32_t x, y, z, w, h, d;
			uint64_t offset;
			uint32_t resource_id, level, stride, layer_stride;
		} t;
		int rc2;

		if (chain->in[0].len < sizeof(req) + sizeof(t)) {
			resp->type = VIRTIO_GPU_RESP_ERR_UNSPEC;
			break;
		}
		memcpy(&t, (char *)chain->in[0].addr + sizeof(req), sizeof(t));

		rc2 = cogpu_vrend_transfer(type == VIRTIO_GPU_CMD_TRANSFER_TO_HOST_3D,
					   t.resource_id, req.ctx_id, t.level,
					   t.stride, t.layer_stride,
					   t.x, t.y, t.z, t.w, t.h, t.d, t.offset);
		if (rc2 == 0) {
			resp->type = VIRTIO_GPU_RESP_OK_NODATA;
			g_stats.transfers++;
		} else {
			resp->type = VIRTIO_GPU_RESP_ERR_UNSPEC;
			logline("  TRANSFER res %u refused (%d)\n", t.resource_id, rc2);
		}
		break;
	}

	case VIRTIO_GPU_CMD_RESOURCE_CREATE_2D: {
		/*
		 * A 2D resource, which used to fall through to the catch-all
		 * and be answered OK while nothing was created.
		 *
		 * The guest makes its cursor and framebuffer this way, attaches
		 * backing to them and renders; with nothing created here,
		 * ATTACH_BACKING was refused by the renderer and every transfer
		 * afterwards failed against a resource that did not exist.
		 *
		 * virtio-gpu's format numbers are gallium's, which is what
		 * virglrenderer wants, so the format passes straight through.
		 */
		struct { uint32_t resource_id, format, width, height; } r2;
		struct virgl_renderer_resource_create_args args;

		if (chain->in[0].len < sizeof(req) + sizeof(r2)) {
			logline("  RESOURCE_CREATE_2D: short header\n");
			resp->type = VIRTIO_GPU_RESP_ERR_UNSPEC;
			break;
		}
		memcpy(&r2, (char *)chain->in[0].addr + sizeof(req), sizeof(r2));

		memset(&args, 0, sizeof(args));
		args.handle	= r2.resource_id;
		args.target	= 2;		/* PIPE_TEXTURE_2D */
		args.format	= r2.format;
		args.bind	= (1 << 1);	/* VIRGL_BIND_RENDER_TARGET */
		args.width	= r2.width;
		args.height	= r2.height;
		args.depth	= 1;
		args.array_size	= 1;

		if (cogpu_vrend_resource_create(&args) != 0) {
			logline("  RESOURCE_CREATE_2D %u (%ux%u fmt %u) refused\n",
				r2.resource_id, r2.width, r2.height, r2.format);
			resp->type = VIRTIO_GPU_RESP_ERR_UNSPEC;
		} else {
			logline("  RESOURCE_CREATE_2D %u: %ux%u fmt %u\n",
				r2.resource_id, r2.width, r2.height, r2.format);
			resp->type = VIRTIO_GPU_RESP_OK_NODATA;
		}
		break;
	}

	case VIRTIO_GPU_CMD_RESOURCE_CREATE_BLOB:
	case VIRTIO_GPU_CMD_RESOURCE_ASSIGN_UUID:
	case VIRTIO_GPU_CMD_GET_EDID:
		/*
		 * Not implemented, and said so.
		 *
		 * These used to reach the catch-all and be answered OK, which
		 * is how the guest came to hold blob resources that were never
		 * created here -- and then every transfer naming one failed,
		 * far from the cause. A guest copes with a refusal; it cannot
		 * cope with being told something exists when it does not.
		 */
		logline("  cmd 0x%04x is not implemented; refusing rather than"
			" pretending\n", type);
		resp->type = VIRTIO_GPU_RESP_ERR_UNSPEC;
		break;

	default:
		logline("  unknown command 0x%04x, answering OK\n", type);
		resp->type = VIRTIO_GPU_RESP_OK_NODATA;
		break;
	}

	if (req.flags & VIRTIO_GPU_FLAG_FENCE) {
		resp->flags    = VIRTIO_GPU_FLAG_FENCE;
		resp->fence_id = req.fence_id;
		resp->ctx_id   = req.ctx_id;
		resp->ring_idx = req.ring_idx;
		g_stats.fenced++;
	}

	g_stats.requests++;
	g_stats.by_type[(type >> 8) & 0xf]++;

	if (verbose)
		logline("  req type 0x%04x%s -> 0x%04x\n", type,
		       (req.flags & VIRTIO_GPU_FLAG_FENCE) ? " (fenced)" : "",
		       resp->type);

	return written;
}

/* ------------------------------------------------------------------- main */

int main(int argc, char **argv)
{
	co_manager_handle_t	handle;
	struct co_vgpu_io_abi  *io = NULL;
	struct cogpu_vring	vq[CO_VGPU_NUM_VQS];
	unsigned int		last_kick[CO_VGPU_NUM_VQS] = { 0, 0 };
	unsigned long long	vgpu_va = 0;
	int			i, once = 0, idle = 0;
	unsigned long long	sweeps = 0;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--verbose"))
			verbose = 1;
		else if (!strcmp(argv[i], "--once"))
			once = 1;
		else if (!strcmp(argv[i], "--selftest"))
			return cogpu_vring_selftest();
		else {
			logline("usage: cogpu-daemon [--verbose] [--once] [--selftest]\n");
			return 2;
		}
	}

	/*
	 * Unbuffered, because this daemon does not exit.
	 *
	 * Redirected to a file, stdout is fully buffered, and a program that
	 * runs forever never flushes it -- so the log stays empty and the
	 * daemon looks hung when it is working perfectly. This cost three
	 * minutes of staring at a zero-byte file.
	 */
	setvbuf(stdout, NULL, _IONBF, 0);

	/*
	 * Beside the images, wherever those are -- not a hardcoded C:.
	 *
	 * The installer puts images under %SystemDrive%\MoCoLinux, and the system
	 * drive is not C: on every install: this project's XP x64 boots from E:,
	 * so a literal "C:\MoCoLinux\..." opened nothing, fopen returned NULL,
	 * and the daemon ran perfectly while appearing to produce no log at all.
	 * Diagnosing a silent daemon is exactly what the log exists for.
	 *
	 * Falling back to the current directory rather than giving up, because a
	 * log in the wrong place still beats no log.
	 */
	{
		char path[MAX_PATH];
		char drive[8];
		DWORD n = GetEnvironmentVariableA("SystemDrive", drive, sizeof(drive));

		if (n > 0 && n < sizeof(drive))
			_snprintf(path, sizeof(path), "%s\\MoCoLinux\\cogpu-daemon.log",
				  drive);
		else
			_snprintf(path, sizeof(path), "cogpu-daemon.log");
		path[sizeof(path) - 1] = '\0';

		g_log = fopen(path, "w");
		if (!g_log)
			g_log = fopen("cogpu-daemon.log", "w");
	}

	/*
	 * virglrenderer's own diagnostics.
	 *
	 * Every EINVAL it returns comes with a vrend_report_context_error
	 * naming the exact check that failed -- illegal resource, transfer
	 * bounds, and so on -- written to stderr. Started hidden from a .vbs,
	 * this process's stderr goes to a console nobody sees, so the renderer
	 * was explaining itself into nothing while its failures were guessed
	 * at from the guest's "response 0x1200".
	 */
	{
		FILE *e = freopen("C:\\MoCoLinux\\cogpu-vrend.log", "w", stderr);

		if (e)
			setvbuf(e, NULL, _IONBF, 0);
	}

	logline("cogpu-daemon: the host side of the guest's GPU\n\n");

	/*
	 * The ring walker, proved against scratch memory before it is pointed
	 * at a live guest. Never-executed code is untested code, and this
	 * particular code gets to dereference addresses the guest chose.
	 */
	if (cogpu_vring_selftest() != 0) {
		logline("refusing to start: the ring walker failed its own test\n");
		return 1;
	}

	/*
	 * The renderer, before anything else that matters. If it will not come
	 * up there is no point servicing a ring: the guest would get OK to
	 * everything and render nothing, which is worse than a device that
	 * refuses honestly.
	 */
	if (cogpu_vrend_init(NULL, NULL) != 0) {
		logline("virglrenderer would not initialise -- no GPU today\n");
		return 1;
	}
	logline("renderer: %s\n", cogpu_vrend_renderer());
	{
		unsigned int v = 0, sz = 0;

		cogpu_vrend_capset(1, &v, &sz);
		logline("virgl capset: version %u, %u bytes\n", v, sz);
	}

	handle = co_os_manager_open();
	g_wake_handle = handle;		/* for cogpu_ring_doorbell(), incl. the X wire */
	if (!handle) {
		logline("cannot open the driver -- is it started?\n");
		return 1;
	}
	InitializeCriticalSection(&g_slice_lock);
	g_slice_lock_ready = 1;

	/*
	 * Wait for the published transport, translate it once, then map only the
	 * slice containing it. This is the first working-set page; vrings, command
	 * buffers, resources and X-wire pages are added by resolve_gpa on demand.
	 *
	 * moco-boot.vbs starts this a fraction of a second after the boot
	 * daemon. A nonzero symbol can precede KRAM readiness by a small window,
	 * so all three operations are retried together for one bounded minute.
	 */
	{
		int tries;
		unsigned long long gpa = 0;
		unsigned int last_magic = 0;
		int gpa_valid = 0;

		for (tries = 0; tries < 120; tries++) {
			MEMORY_BASIC_INFORMATION mbi;
			if (!vgpu_va)
				co_manager_vgpu_address(handle, &vgpu_va);
			if (vgpu_va && !gpa_valid &&
			    CO_OK(co_manager_kvirt_to_phys(handle, vgpu_va, &gpa)))
				gpa_valid = 1;
			if (gpa_valid && !io)
				io = resolve_gpa(NULL, gpa, sizeof(*io));

			if (io) {
				ZeroMemory(&mbi, sizeof(mbi));
				if (VirtualQuery(io, &mbi, sizeof(mbi)) == sizeof(mbi) &&
				    mbi.State == MEM_COMMIT &&
				    !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) &&
				    (size_t)((char *)mbi.BaseAddress + mbi.RegionSize -
					     (char *)io) >= sizeof(*io)) {
					last_magic = io->magic;
					if (last_magic == CO_VGPU_MAGIC)
						break;
				}
			}

			if (tries == 0)
				logline("waiting for a guest GPU transport...\n");
			Sleep(500);
		}

		if (tries >= 120) {
			if (!vgpu_va)
				logline("no guest GPU transport after 60 s (old kernel?)\n");
			else if (!gpa_valid)
				logline("could not translate transport va 0x%llx\n", vgpu_va);
			else if (!io)
				logline("could not map transport pa 0x%llx cleanly\n", gpa);
			else
				logline("transport magic stayed 0x%08x, expected 0x%08x\n",
					last_magic, (unsigned int)CO_VGPU_MAGIC);
			co_manager_kunmap(handle, NULL);
			co_os_manager_close(handle);
			return 1;
		}

		logline("transport mapped on demand at guest pa 0x%llx;"
			" %ld slice(s), %llu MB resident in cogpu\n",
			gpa, (long)g_slice_count, g_slice_bytes >> 20);
	}

	logline("transport at guest va 0x%llx, status 0x%x\n\n",
	       vgpu_va, io->status);

	/*
	 * The X wire, if it can be had.
	 *
	 * Only started once the transport mapping has proved KMAP_RANGE, because a
	 * channel's first act is to map its guest-physical pages through that path.
	 * A failure here is not fatal to anything: the guest's shim falls back
	 * to reaching the X server over slirp exactly as it did before, slowly.
	 */
	cogpu_xwire_start();

	memset(vq, 0, sizeof(vq));
	io->enabled = 1;

	/*
	 * The poll. Hot while there is work, backing off to a sleep when there
	 * is not -- the cadence is a stated design constant rather than an
	 * accident, because the fence latency this rung is measured on is a
	 * property of the transport and must not silently become a property of
	 * an arbitrary sleep.
	 */
	{
		DWORD t0 = GetTickCount(), last = t0;

	for (;;) {
		int did = 0;

		io->host_heartbeat++;
		sweeps++;

		if (GetTickCount() - last > 2000) {
			last = GetTickCount();
			logline("[%lus] status 0x%x q0 num %u kick %u  q1 num %u kick %u"
			       "  served %llu\n",
			       (unsigned long)((last - t0) / 1000),
			       io->status,
			       io->vq[0].num, io->vq[0].kick,
			       io->vq[1].num, io->vq[1].kick,
			       g_stats.requests);
		}

		/*
		 * Queues are set up by the guest during probe, after this
		 * daemon may already be running, so their addresses are picked
		 * up lazily rather than once at startup.
		 */
		for (i = 0; i < CO_VGPU_NUM_VQS; i++) {
			if (vq[i].desc || !io->vq[i].num)
				continue;

			vq[i].num   = io->vq[i].num;
			vq[i].desc  = resolve_gpa(NULL, io->vq[i].desc_gpa,
						  sizeof(struct vring_desc) * vq[i].num);
			vq[i].avail = resolve_gpa(NULL, io->vq[i].avail_gpa, 8);
			vq[i].used  = resolve_gpa(NULL, io->vq[i].used_gpa, 8);

			if (vq[i].desc && vq[i].avail && vq[i].used) {
				/*
				 * Start where the guest is, not at zero.
				 *
				 * This daemon attaches long after the guest has
				 * been running, and is restarted during
				 * development while the guest keeps going.
				 * Starting last_avail at zero leaves it hundreds
				 * of entries behind, and cogpu_vring_pop's
				 * sanity check -- more than a ring's worth
				 * behind means the ring is not trustworthy --
				 * then refuses every request forever. The
				 * symptom is a daemon that polls busily and
				 * serves nothing: kick 259, served 0.
				 *
				 * Joining at USED rather than AVAIL because the
				 * gap between them is the guest's outstanding
				 * work. Skipping it abandons every thread
				 * waiting on those fences; replaying it answers
				 * them and the guest carries on.
				 */
				vq[i].last_avail = vq[i].used->idx;
				vq[i].used_idx	 = vq[i].used->idx;

				logline("queue %d: %u descriptors,"
					" joining at used %u\n",
					i, vq[i].num, vq[i].used_idx);
			} else {
				logline("queue %d: addresses outside guest RAM\n", i);
				vq[i].desc = NULL;
			}
		}

		for (i = 0; i < CO_VGPU_NUM_VQS; i++) {
			struct cogpu_chain chain;

			if (!vq[i].desc)
				continue;

			last_kick[i] = io->vq[i].kick;

			while (cogpu_vring_pop(&vq[i], resolve_gpa, NULL, &chain)) {
				uint32_t written = serve(&chain);

				cogpu_vring_push(&vq[i], &chain, written);
				did = 1;
			}
		}

		if (did) {
			/*
			 * Tell the guest there is something to reap. It calls
			 * vring_interrupt from its cooperative drain points and
			 * wakes whatever slept on the fence -- it never spins,
			 * because it has one processor and spinning would stop
			 * the whole machine.
			 */
			__atomic_thread_fence(__ATOMIC_RELEASE);
			io->used_pending++;

			/*
			 * The other half of the doorbell. The bump above is a
			 * plain store the monitor loop cannot see, and a guest
			 * waiting on a fence is idle by definition -- without
			 * this it slept a whole backoff tick (~10 ms, measured
			 * offscreen at every resolution) per completion. The
			 * ioctl cuts that sleep short; the guest drains at the
			 * idle boundary and wakes the fence waiter. Once per
			 * service pass, not per request, and nothing to do if
			 * it fails -- the tick it would have saved still
			 * happens.
			 */
			co_manager_vgpu_wake(handle);
			idle = 0;

			if (once) {
				logline("\nserved %llu request(s); --once, so stopping\n",
				       g_stats.requests);
				break;
			}
		} else if (++idle > 20000) {
			Sleep(1);
			idle = 20000;
		} else {
			__builtin_ia32_pause();
		}

		if (io->status == 0 && sweeps > 1000000) {
			logline("the guest reset the device; exiting\n");
			break;
		}
	}

	}

	logline("\nrequests %llu, fenced %llu, refused %llu, sweeps %llu\n",
	       g_stats.requests, g_stats.fenced, g_stats.refused, sweeps);
	logline("3D submits %llu, %llu command bytes, %llu fences from the renderer\n",
	       g_stats.submits, g_stats.cmd_bytes, cogpu_vrend_fences());
	logline("resources %llu, transfers %llu, %llu MB of guest-backed storage\n",
	       g_stats.resources, g_stats.transfers, g_stats.backing_bytes >> 20);

	io->enabled = 0;
	co_manager_kunmap(handle, NULL);
	co_os_manager_close(handle);
	return 0;
}
