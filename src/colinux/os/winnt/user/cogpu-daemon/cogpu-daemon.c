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
#define VIRTIO_GPU_CMD_GET_DISPLAY_INFO		0x0100
#define VIRTIO_GPU_CMD_GET_CAPSET_INFO		0x0102
#define VIRTIO_GPU_CMD_GET_CAPSET		0x0103
#define VIRTIO_GPU_CMD_GET_EDID			0x0104
#define VIRTIO_GPU_CMD_CTX_CREATE		0x0200
#define VIRTIO_GPU_CMD_CTX_DESTROY		0x0201
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

	if (chain->in_count == 0 || chain->in[0].len < sizeof(req) ||
	    chain->out_count == 0 || chain->out[0].len < sizeof(*resp)) {
		g_stats.refused++;
		return 0;
	}

	memcpy(&req, chain->in[0].addr, sizeof(req));
	type = req.type;

	resp = (struct virtio_gpu_ctrl_hdr *)chain->out[0].addr;
	memset(resp, 0, sizeof(*resp));

	switch (type) {
	case VIRTIO_GPU_CMD_GET_DISPLAY_INFO:
		/*
		 * No scanouts, so the reply is a header and a block of zeroed
		 * display modes -- which is exactly "no displays", and what a
		 * render-only device should say.
		 */
		resp->type = VIRTIO_GPU_RESP_OK_DISPLAY_INFO;
		if (chain->out[0].len > sizeof(*resp))
			memset((char *)resp + sizeof(*resp), 0,
			       chain->out[0].len - sizeof(*resp));
		break;

	case VIRTIO_GPU_CMD_GET_CAPSET_INFO:
	case VIRTIO_GPU_CMD_GET_CAPSET:
		/*
		 * Refused for now, and honestly: capsets describe what the
		 * renderer can do, and there is no renderer yet. Advertising a
		 * capability here would be a lie Mesa would then act on.
		 */
		resp->type = VIRTIO_GPU_RESP_ERR_UNSPEC;
		break;

	case VIRTIO_GPU_CMD_CTX_CREATE:
	case VIRTIO_GPU_CMD_CTX_DESTROY:
	case VIRTIO_GPU_CMD_SUBMIT_3D:
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

	return sizeof(*resp);
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

	if (!CO_OK(co_manager_kmap(handle, 0, g_map))) {
		logline("KMAP failed -- is a guest running?\n");
		co_os_manager_close(handle);
		return 1;
	}
	logline("mapped %lu slices, %llu MB of guest RAM\n",
	       g_map->count, g_map->total_bytes >> 20);

	/*
	 * Where the transport structure is. The loader knows, and puts it in
	 * the boot record; this daemon asks the driver for it rather than
	 * guessing, because the address moves with every kernel build.
	 */
	if (!CO_OK(co_manager_vgpu_address(handle, &vgpu_va)) || !vgpu_va) {
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

	io->enabled = 0;
	co_manager_kunmap(handle, NULL);
	co_os_manager_close(handle);
	return 0;
}
