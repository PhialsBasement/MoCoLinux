/*
 * cogpu-daemon -- the host side of the guest's GPU.
 *
 * A sibling of colinux-slirp-net-daemon.exe: an ordinary Windows program that
 * services one of the guest's device rings. It runs in userspace because that
 * is where WGL and the NVIDIA driver are, and it will eventually embed
 * virglrenderer. This is the transport half of it -- everything up to and
 * including a fenced request going round -- with the renderer stubbed.
 *
 * How it reaches the guest, and why that is unusual: it does not. There is no
 * copy in or out. R3 maps the guest's RAM into this process once at startup,
 * so a descriptor's guest physical address becomes a pointer by arithmetic and
 * the rings are simply memory. A vertex buffer the guest fills is readable
 * here at DDR speed the instant the guest has written it.
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

static void logline(const char *fmt, ...)
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

/*
 * The R3 windows, and the map from guest physical to a pointer here.
 *
 * Linear search over the ranges, with the last hit remembered. There are on
 * the order of a hundred and thirty of them and consecutive descriptors are
 * overwhelmingly in the same one, so the cache answers almost every time; a
 * binary search would be tidier and is not yet worth the code.
 */
static co_manager_ioctl_kmap_t *g_map;
static int			g_last_hit;

static void *resolve_gpa(void *ctx, uint64_t gpa, uint32_t len)
{
	int i, n;

	(void)ctx;
	if (!g_map)
		return NULL;

	n = (int)g_map->count;

	if (g_last_hit < n) {
		co_kmap_range_t *r = &g_map->range[g_last_hit];

		if (gpa >= r->pa && gpa + len <= r->pa + r->bytes)
			return (void *)(size_t)(r->user_va + (gpa - r->pa));
	}

	for (i = 0; i < n; i++) {
		co_kmap_range_t *r = &g_map->range[i];

		if (gpa >= r->pa && gpa + len <= r->pa + r->bytes) {
			g_last_hit = i;
			return (void *)(size_t)(r->user_va + (gpa - r->pa));
		}
	}

	/*
	 * Not inside guest RAM. This is not an assertion failure -- the guest
	 * wrote that address and the guest can be wrong -- so it is refused
	 * and counted, never followed.
	 */
	return NULL;
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
					   c.namelen > 63 ? 63 : c.namelen) == 0)
			resp->type = VIRTIO_GPU_RESP_OK_NODATA;
		else
			resp->type = VIRTIO_GPU_RESP_ERR_UNSPEC;
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
		const char *cmds;
		uint32_t avail;

		if (chain->in[0].len < sizeof(req) + sizeof(sub)) {
			resp->type = VIRTIO_GPU_RESP_ERR_UNSPEC;
			break;
		}
		memcpy(&sub, (char *)chain->in[0].addr + sizeof(req), sizeof(sub));

		cmds  = (const char *)chain->in[0].addr + sizeof(req) + sizeof(sub);
		avail = chain->in[0].len - sizeof(req) - sizeof(sub);
		if (sub.size > avail && chain->in_count > 1) {
			/* the stream is in the next descriptor */
			cmds  = (const char *)chain->in[1].addr;
			avail = chain->in[1].len;
		}
		if (sub.size > avail) {
			resp->type = VIRTIO_GPU_RESP_ERR_UNSPEC;
			break;
		}

		if (cogpu_vrend_submit(req.ctx_id, cmds, sub.size) == 0) {
			resp->type = VIRTIO_GPU_RESP_OK_NODATA;
			g_stats.submits++;
			g_stats.cmd_bytes += sub.size;
		} else {
			resp->type = VIRTIO_GPU_RESP_ERR_UNSPEC;
		}
		break;
	}

	default:
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
	g_log = fopen("C:\\MoCoLinux\\cogpu-daemon.log", "w");

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
	if (!handle) {
		logline("cannot open the driver -- is it started?\n");
		return 1;
	}

	g_map = malloc(sizeof(*g_map));
	if (!g_map) {
		logline("out of memory\n");
		return 1;
	}

	/*
	 * Wait for a guest rather than requiring one.
	 *
	 * moco-boot.vbs starts this a fraction of a second after the boot
	 * daemon, and guest RAM does not exist until that daemon has allocated
	 * it -- so a daemon that demanded a guest at startup exited instantly
	 * every time it was launched the normal way, and only ever worked when
	 * started by hand afterwards. The transport was written to tolerate a
	 * LATE daemon; this is the other half, tolerating an early one.
	 *
	 * Bounded, because a daemon that waits forever for a guest that is
	 * never coming is a process someone has to find and kill.
	 */
	{
		int tries;

		for (tries = 0; tries < 120; tries++) {
			if (CO_OK(co_manager_kmap(handle, 0, g_map)))
				break;
			if (tries == 0)
				logline("waiting for a guest...\n");
			Sleep(500);
		}

		if (tries >= 120) {
			logline("no guest after 60 s -- exiting\n");
			co_os_manager_close(handle);
			return 1;
		}
	}
	logline("mapped %lu slices, %llu MB of guest RAM\n",
	       g_map->count, g_map->total_bytes >> 20);

	/*
	 * Where the transport structure is. The loader knows, and puts it in
	 * the boot record; this daemon asks the driver for it rather than
	 * guessing, because the address moves with every kernel build.
	 */
	{
		int tries;

		for (tries = 0; tries < 120; tries++) {
			if (CO_OK(co_manager_vgpu_address(handle, &vgpu_va)) && vgpu_va)
				break;
			Sleep(500);
		}
	}

	if (!vgpu_va) {
		logline("the guest has no vgpu transport (old kernel?)\n");
		co_manager_kunmap(handle, NULL);
		co_os_manager_close(handle);
		return 1;
	}

	/*
	 * And the structure itself, through the same windows as everything
	 * else -- it lives in the guest's .bss, which is guest RAM, which is
	 * mapped. Its address is a guest VIRTUAL address though, so the driver
	 * has to translate it once; after that this is a plain pointer.
	 */
	{
		unsigned long long gpa = 0;

		if (!CO_OK(co_manager_kvirt_to_phys(handle, vgpu_va, &gpa))) {
			logline("could not resolve the transport's address\n");
			co_manager_kunmap(handle, NULL);
			co_os_manager_close(handle);
			return 1;
		}
		io = (struct co_vgpu_io_abi *)resolve_gpa(NULL, gpa, sizeof(*io));
	}

	if (!io || io->magic != CO_VGPU_MAGIC) {
		logline("the transport structure is not where it should be\n");
		co_manager_kunmap(handle, NULL);
		co_os_manager_close(handle);
		return 1;
	}

	logline("transport at guest va 0x%llx, status 0x%x\n\n",
	       vgpu_va, io->status);

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
				logline("queue %d: %u descriptors\n", i, vq[i].num);
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

	io->enabled = 0;
	co_manager_kunmap(handle, NULL);
	co_os_manager_close(handle);
	return 0;
}
