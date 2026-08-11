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
 *   cogpu-daemon.exe [--verbose] [--selftest] [--once] [--present-probe]
 *                    [--present-r1] [--present-r2]
 */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>

#include <colinux/common/common.h>
#include <colinux/common/debug.h>
#include <colinux/user/manager.h>
#include <colinux/os/user/manager.h>
#include <colinux/os/alloc.h>

#include "vring.h"
#include "vrend.h"
#include "present.h"
#include "copresent.h"
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
static int present_r1;
static int present_r2;

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

/*
 * The window arena, from the driver via the VGPU ioctl at startup. MAP_BLOB
 * turns a guest-chosen offset into guest pa = base + offset; zero top means
 * the driver predates windows and every MAP_BLOB is refused.
 */
static unsigned long long g_window_base, g_window_top;

/*
 * Mapped blobs: what UNMAP_BLOB needs to give back. Slots are keyed by
 * resource id; a zero id is a free slot, and resource id zero is invalid in
 * the protocol so the sentinel cannot collide.
 */
struct cogpu_blob_map {
	uint32_t	   res_id;
	unsigned long long pseudo_pa;
	unsigned long long bytes;
	/* The mapping MAP_BLOB created and still holds; a second
	 * resource_map on a live blob is refused, so a consumer that wants
	 * the pixels (MOCO_PRESENT) reads through this one. */
	void		  *host_va;
};
/*
 * GROWN ON DEMAND, not a fixed 1024.
 *
 * A fixed table was sized by what vkcube needed, and the first real workload
 * walked straight past it: Unigine Heaven mapped more than 1024 blobs at
 * once, so MAP_BLOB began answering ERR_UNSPEC, and every later UNMAP_BLOB
 * for one of those untracked resources answered ERR too -- 164 refusals in
 * matched pairs, which the guest reported as
 *
 *   [drm] *ERROR* response 0x1200 (command 0x208)   <- MAP_BLOB
 *   [drm] *ERROR* response 0x1200 (command 0x209)   <- UNMAP_BLOB
 *
 * and the application as a crash. The entries are 32 bytes; a workload that
 * genuinely holds a million mappings is welcome to 32 MB of table.
 */
static struct cogpu_blob_map *g_blob_map;
static unsigned int	      g_blob_map_slots;

/* Defined below; the table's growth wants to report itself. */
void logline(const char *fmt, ...);

static struct cogpu_blob_map *blob_map_find(uint32_t res_id)
{
	unsigned int i;

	if (res_id == 0)
		return NULL;
	for (i = 0; i < g_blob_map_slots; i++)
		if (g_blob_map[i].res_id == res_id)
			return &g_blob_map[i];
	return NULL;
}

static int blob_map_track(uint32_t res_id, unsigned long long pseudo_pa,
			  unsigned long long bytes, void *host_va)
{
	unsigned int i;

	for (i = 0; i < g_blob_map_slots; i++) {
		if (g_blob_map[i].res_id == 0)
			goto have_slot;
	}

	{
		/* Full: double it (1024 to start), keeping every live entry. */
		unsigned int want = g_blob_map_slots ? g_blob_map_slots * 2 : 1024;
		struct cogpu_blob_map *bigger =
			realloc(g_blob_map, want * sizeof(*bigger));

		if (!bigger) {
			logline("  MAP_BLOB: cannot grow the tracking table"
				" past %u entries\n", g_blob_map_slots);
			return -1;
		}
		memset(bigger + g_blob_map_slots, 0,
		       (want - g_blob_map_slots) * sizeof(*bigger));
		i = g_blob_map_slots;
		g_blob_map = bigger;
		g_blob_map_slots = want;
		logline("  MAP_BLOB: tracking table grown to %u entries\n",
			want);
	}

have_slot:
	g_blob_map[i].res_id	= res_id;
	g_blob_map[i].pseudo_pa = pseudo_pa;
	g_blob_map[i].bytes	= bytes;
	g_blob_map[i].host_va	= host_va;
	return 0;
}

static void blob_map_forget(uint32_t res_id)
{
	struct cogpu_blob_map *bm = blob_map_find(res_id);

	if (bm)
		bm->res_id = 0;
}

/*
 * Which capset each context was created for. Venus contexts (capset 4) get
 * real per-context fences and deferred completion; everything else keeps the
 * inline path that has carried GL since R4. Mesa's ctx ids are small; one
 * byte per possible id costs nothing.
 */
#define COGPU_MAX_CTX 4096
static uint8_t g_ctx_capset[COGPU_MAX_CTX];

static int ctx_is_venus(uint32_t ctx_id)
{
	return ctx_id < COGPU_MAX_CTX && g_ctx_capset[ctx_id] == 4;
}

/*
 * The capset the guest ASKED for, kept even when the creation failed.
 *
 * g_ctx_capset above records what exists; this records what was wanted, which
 * is what a rebuild needs.
 */
static uint8_t g_ctx_wanted[COGPU_MAX_CTX];

/*
 * A refusal that repeats is worth seeing once, not a hundred thousand times.
 *
 * One black Steam client wrote 27 MB of identical lines in twenty minutes,
 * pushing the history that explained it out of reach and filling the disk it
 * was on. The first few, then powers of two, keeps the onset, the fact that it
 * is continuing, and the total -- which is all any of them ever told us.
 */
static int refusal_worth_logging(unsigned long long *seen)
{
	unsigned long long n = ++*seen;

	if (n <= 8)
		return 1;
	return (n & (n - 1)) == 0;
}

static unsigned long long g_submit_refusals;
static unsigned long long g_transfer_refusals;
static unsigned long long g_refusals;

/*
 * Requests whose completion is a fence that has not signalled yet. The chain
 * is already answered (response header written), but the used-ring element
 * is withheld until vkr says the fence retired -- that withholding IS the
 * virtio-gpu fence contract, and the guest's dma_fence sleeps on it.
 */
struct cogpu_parked {
	int	 vq;		/* -1 = free slot */
	uint16_t head;
	uint32_t written;
	uint32_t ctx_id;
	uint32_t ring_idx;
	uint64_t fence_id;
};
static struct cogpu_parked g_parked[512];
static int g_parked_init_done;

/*
 * Fence signals crossing from vkr's ring threads. write_context_fence runs
 * over there; the vrings belong to the main thread. A small locked ring
 * carries the (ctx, ring, fence) triples across, and the main loop drains
 * it every pass. Fences per ring signal in order, so a triple stands for
 * "everything on this ring up to fence_id".
 */
static CRITICAL_SECTION g_fence_lock;
static struct { uint32_t ctx_id, ring_idx; uint64_t fence_id; } g_fence_sig[256];
static unsigned g_fence_sig_head, g_fence_sig_tail;
static volatile LONG g_fence_sig_dropped;

static void cogpu_ctx_fence_signal(uint32_t ctx_id, uint32_t ring_idx,
				   uint64_t fence_id)
{
	EnterCriticalSection(&g_fence_lock);
	if (g_fence_sig_head - g_fence_sig_tail <
	    sizeof(g_fence_sig) / sizeof(g_fence_sig[0])) {
		unsigned slot = g_fence_sig_head++ %
			(sizeof(g_fence_sig) / sizeof(g_fence_sig[0]));
		g_fence_sig[slot].ctx_id   = ctx_id;
		g_fence_sig[slot].ring_idx = ring_idx;
		g_fence_sig[slot].fence_id = fence_id;
	} else {
		/* Never silent: a dropped signal is a hung guest fence. */
		InterlockedIncrement(&g_fence_sig_dropped);
	}
	LeaveCriticalSection(&g_fence_lock);
}

static int cogpu_park(int vq, uint16_t head, uint32_t written,
		      uint32_t ctx_id, uint32_t ring_idx, uint64_t fence_id)
{
	unsigned i;

	for (i = 0; i < sizeof(g_parked) / sizeof(g_parked[0]); i++) {
		if (g_parked[i].vq < 0) {
			g_parked[i].vq	     = vq;
			g_parked[i].head     = head;
			g_parked[i].written  = written;
			g_parked[i].ctx_id   = ctx_id;
			g_parked[i].ring_idx = ring_idx;
			g_parked[i].fence_id = fence_id;
			return 0;
		}
	}
	return -1;
}

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
/*
 * MAP/UNMAP_BLOB are 3D commands, after SUBMIT_3D -- NOT 0x010d/0x010e in
 * the 2d block, which is where a first guess put them. Wrong numbers here
 * meant real MAP_BLOBs fell to the catch-all and were answered OK with no
 * window and no map_info: the guest was told a mapping existed that did
 * not, and vn's first instance died of it far away, as ERROR_OUT_OF_HOST_
 * MEMORY from vkCreateInstance.
 */
#define VIRTIO_GPU_CMD_RESOURCE_MAP_BLOB	0x0208
#define VIRTIO_GPU_CMD_RESOURCE_UNMAP_BLOB	0x0209
/* MoCoLinux device command; transcribed from uapi/linux/virtio_gpu.h. */
#define VIRTIO_GPU_CMD_MOCO_PRESENT		0x0400
/*
 * The daemon-private virgl resources Venus frames are bridged through, one
 * PER WINDOW -- ids far above anything the guest kernel allocates.
 *
 * Per window, not one global: the GL path gives every drawable its own
 * buffers, and a single shared bridge would have two Vulkan clients of
 * different sizes destroying and recreating the same resource every frame,
 * each showing the other's pixels in between.
 */
#define MOCO_BRIDGE_RES_BASE			0x4d4f4300u
#define MOCO_BRIDGE_SLOTS			8

static struct moco_bridge_slot {
	uint32_t xid;
	uint32_t ctx_id;		/* the guest context that presents here */
	uint32_t width, height;
	unsigned long long used;	/* for LRU eviction */
} g_bridge[MOCO_BRIDGE_SLOTS];
static unsigned long long g_bridge_clock;

/*
 * Release every bridge a dying guest context owned: overlay first (so the
 * last frame leaves the screen), then the renderer resource.
 *
 * Without this the final frame of every Vulkan client stayed on screen
 * forever after the client exited -- the GL path never had that problem
 * because the guest owns its displayed resources there and unrefs them,
 * which is what removes the overlay. Bridge resources are the daemon's, so
 * nothing was ever unref'd and nothing ever took the picture down.
 */
static void moco_bridge_release_ctx(uint32_t ctx_id)
{
	unsigned int i;

	for (i = 0; i < MOCO_BRIDGE_SLOTS; i++) {
		uint32_t res_id;

		if (g_bridge[i].xid == 0 || g_bridge[i].ctx_id != ctx_id)
			continue;

		res_id = MOCO_BRIDGE_RES_BASE + i;
		logline("  MOCO_PRESENT: ctx %u gone, releasing slot %u"
			" (XID 0x%x)\n", ctx_id, i, g_bridge[i].xid);
		cogpu_present_r1_resource_unref(res_id);
		cogpu_present_r2_resource_unref(res_id);
		if (g_bridge[i].width)
			cogpu_vrend_resource_unref(res_id);
		memset(&g_bridge[i], 0, sizeof(g_bridge[i]));
	}
}

/*
 * The slot for this window, sized for this frame. Returns the virgl
 * resource id, or 0 if the resource could not be created.
 */
static uint32_t moco_bridge_resource(uint32_t xid, uint32_t ctx_id,
				     uint32_t width, uint32_t height)
{
	struct virgl_renderer_resource_create_args a;
	unsigned int i, pick = 0;
	uint32_t res_id;

	for (i = 0; i < MOCO_BRIDGE_SLOTS; i++) {
		if (g_bridge[i].xid == xid) {
			pick = i;
			goto have_slot;
		}
	}
	/* Free slot first, else the least recently used. */
	for (i = 0; i < MOCO_BRIDGE_SLOTS; i++) {
		if (g_bridge[i].xid == 0) {
			pick = i;
			goto have_slot;
		}
		if (g_bridge[i].used < g_bridge[pick].used)
			pick = i;
	}
	if (g_bridge[pick].width)
		cogpu_vrend_resource_unref(MOCO_BRIDGE_RES_BASE + pick);
	g_bridge[pick].width = g_bridge[pick].height = 0;

have_slot:
	res_id = MOCO_BRIDGE_RES_BASE + pick;
	g_bridge[pick].xid = xid;
	g_bridge[pick].ctx_id = ctx_id;
	g_bridge[pick].used = ++g_bridge_clock;

	if (g_bridge[pick].width == width &&
	    g_bridge[pick].height == height)
		return res_id;

	if (g_bridge[pick].width)
		cogpu_vrend_resource_unref(res_id);

	memset(&a, 0, sizeof(a));
	a.handle     = res_id;
	a.target     = 2;		/* PIPE_TEXTURE_2D */
	a.format     = 1;		/* B8G8R8A8_UNORM */
	a.bind	     = 1 << 1;		/* PIPE_BIND_RENDER_TARGET */
	a.width	     = width;
	a.height     = height;
	a.depth	     = 1;
	a.array_size = 1;
	if (cogpu_vrend_resource_create(&a) != 0) {
		g_bridge[pick].width = g_bridge[pick].height = 0;
		return 0;
	}
	g_bridge[pick].width = width;
	g_bridge[pick].height = height;
	logline("  MOCO_PRESENT: bridge slot %u res %u for XID 0x%x %ux%u\n",
		pick, res_id, xid, width, height);
	return res_id;
}

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
/* 0x1105 is RESP_OK_RESOURCE_UUID; a first guess skipped it, the guest
 * kernel classified the mapping as failed, and vn died 8 ms later in mmap.
 * Enum values are transcribed from uapi headers, never counted by hand. */
#define VIRTIO_GPU_RESP_OK_MAP_INFO		0x1106
#define VIRTIO_GPU_RESP_ERR_UNSPEC		0x1200

#define VIRTIO_GPU_FLAG_FENCE			(1 << 0)
#define VIRTIO_GPU_FLAG_INFO_RING_IDX		(1 << 1)

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
/*
 * The one request whose completion must wait. Set by serve() when a fenced
 * request belongs to a Venus context; the loop parks the chain instead of
 * publishing it, and the fence callback releases it later.
 */
static struct {
	int	 pending;
	uint32_t ctx_id, ring_idx;
	uint64_t fence_id;
} g_park_req;

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
		/*
		 * Index 2 is Venus (capset id 4). When the renderer came up
		 * without Vulkan, get_cap_set answers version 0 / size 0 for
		 * it and Mesa moves on -- an honest absence, no special case.
		 */
		info->capset_id = (q.capset_index == 0) ? 1 :
				  (q.capset_index == 1) ? 2 : 4;
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

		/* Capset 0 means the legacy default, VIRGL2 -- the same rule
		 * vrend.c applies when creating it. Recorded before the
		 * attempt, because a failed attempt is exactly when the wanted
		 * capset has to survive: the guest will not ask twice. */
		if (req.ctx_id < COGPU_MAX_CTX)
			g_ctx_wanted[req.ctx_id] =
				(c.ctx_init & 0xff) ? (c.ctx_init & 0xff) : 2;

		/*
		 * Whatever used to hold this id is finished. Say so before
		 * making the new one.
		 *
		 * virtio-gpu recycles context ids aggressively -- the guest
		 * frees the id as soon as it QUEUES the destroy -- so the same
		 * number comes back for an unrelated client moments later, and
		 * on this box it comes back with a different capset: ctx 3 was
		 * a Venus (capset 4) context, was destroyed, and was recreated
		 * as GL (capset 2) three lines later. That is the id Steam's
		 * renderer then submitted into for 66,000 refusals.
		 *
		 * virglrenderer will not remake an id it still knows:
		 * virgl_renderer_context_create_with_flags() looks it up first
		 * and, finding one, either returns success having created
		 * nothing or refuses outright on a capset mismatch. Either way
		 * the guest ends up submitting a GL stream at whatever the id
		 * used to be, and the failure is silent -- the refusal happens
		 * before the decoder, so the renderer never prints the one
		 * message that would have explained it.
		 *
		 * Destroying first makes CTX_CREATE mean what the guest means
		 * by it. It is a no-op for an id the renderer does not hold.
		 */
		cogpu_vrend_ctx_destroy(req.ctx_id);

		if (cogpu_vrend_ctx_create(req.ctx_id, c.name,
					   c.namelen > 63 ? 63 : c.namelen,
					   c.ctx_init) == 0) {
			if (req.ctx_id < COGPU_MAX_CTX)
				g_ctx_capset[req.ctx_id] =
					g_ctx_wanted[req.ctx_id];
			resp->type = VIRTIO_GPU_RESP_OK_NODATA;
			/*
			 * Always, not behind --verbose. Contexts are a handful
			 * per run, so this costs nothing, and without it the
			 * question "did this context ever exist, and when did
			 * it stop" has no answer anywhere -- which is the
			 * question every one of these failures turns into.
			 */
			logline("  ctx %u created, capset %u\n", req.ctx_id,
				req.ctx_id < COGPU_MAX_CTX
					? g_ctx_wanted[req.ctx_id]
					: (c.ctx_init & 0xff));
		} else {
			/*
			 * Loud, and always -- not behind --verbose.
			 *
			 * This is the failure the guest cannot see: CTX_CREATE
			 * is fire-and-forget, so ERR_UNSPEC goes into a response
			 * nobody reads and the next symptom is thousands of
			 * refusals a long way from here. There is no repairing
			 * it later either -- a GL context cannot be recreated
			 * behind a client that already holds objects in it --
			 * so this line is the only warning that exists.
			 */
			logline("  ctx %u REFUSED at create (capset %u); the"
				" guest will not be told\n",
				req.ctx_id,
				req.ctx_id < COGPU_MAX_CTX
					? g_ctx_wanted[req.ctx_id] : 0);
			resp->type = VIRTIO_GPU_RESP_ERR_UNSPEC;
		}
		break;
	}

	case VIRTIO_GPU_CMD_CTX_DESTROY:
		/* Overlays first: a bridge outliving its context is the final
		 * frame stuck on screen after the client is gone. */
		moco_bridge_release_ctx(req.ctx_id);
		cogpu_vrend_ctx_destroy(req.ctx_id);
		if (req.ctx_id < COGPU_MAX_CTX)
			g_ctx_capset[req.ctx_id] = 0;
		/* The other half of the lifetime, and the one that answers
		 * "was it destroyed under us". */
		logline("  ctx %u destroyed\n", req.ctx_id);
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
		/*
		 * Zero bytes is not malformed -- it is a fence-only
		 * submission, which is how an empty vkQueueSubmit arrives.
		 * Refusing it looked harmless because the error response
		 * still carried the fence flag and the guest treated THAT as
		 * the completion: every empty submit's fence signalled
		 * early, through an error, with the renderer never asked.
		 * The fence tail below handles it properly -- for a Venus
		 * context that means a real vkr fence, parked and published
		 * in order.
		 */
		if (sub.size == 0) {
			resp->type = VIRTIO_GPU_RESP_OK_NODATA;
			break;
		}

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
			struct cogpu_vrend_submit_info submit_info;
			int rc2 = cogpu_vrend_submit(req.ctx_id, cmds, sub.size,
				present_r1 ? &submit_info : NULL);

			if (rc2 == 0) {
				unsigned int hint_index;

				resp->type = VIRTIO_GPU_RESP_OK_NODATA;
				g_stats.submits++;
				g_stats.cmd_bytes += sub.size;

				/* Modern Mesa places readback in TRANSFER3D commands inside
				 * the submitted virgl stream, not in the older outer virtio
				 * TRANSFER_FROM_HOST_3D request. The parser in vrend.c reports
				 * only host-to-guest sources; the presenter then applies the
				 * resource-type and X-window extent checks. */
				for (hint_index = 0; present_r1 &&
				     hint_index < submit_info.present_hint_count;
				     hint_index++) {
					const struct cogpu_vrend_present_hint *hint =
						&submit_info.present_hints[hint_index];
					struct cogpu_vrend_resource_info info;
					int present_rc;

					if (cogpu_vrend_resource_info(hint->resource_id,
								     &info) != 0)
						continue;
					present_rc = cogpu_present_r1_frame(&info,
						hint->level, hint->x, hint->y,
						hint->width, hint->height);
					if (present_rc < 0) {
						logline("present R1: disabling after a"
							" context/window failure\n");
						cogpu_present_r1_fini();
						present_r1 = 0;
					}
				}
			} else {
				resp->type = VIRTIO_GPU_RESP_ERR_UNSPEC;
				if (refusal_worth_logging(&g_submit_refusals))
					logline("  SUBMIT_3D ctx %u, %u bytes,"
						" refused (%d) [%llu total]\n",
						req.ctx_id, sub.size, rc2,
						g_submit_refusals);
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
		 *
		 * Sized for real applications, not just probes: Firefox's
		 * texture-atlas staging arrives as one ATTACH_BACKING with a
		 * page entry per 4 KB -- a 16 MB atlas is 4096 entries. The
		 * old 2048-entry cap refused that WITHOUT LOGGING; the
		 * resource then existed with no backing, the first
		 * COPY_TRANSFER3D through it poisoned the whole context, and
		 * every subsequent draw died (found 2026-08-10 after a night
		 * of the browser black-screening). A refusal must never be
		 * silent again.
		 */
		static struct iovec iov[16384];
		const char *p;
		uint32_t i;
		int bad = 0, niov = 0;

		if (chain->in[0].len < sizeof(req) + sizeof(a)) {
			resp->type = VIRTIO_GPU_RESP_ERR_UNSPEC;
			break;
		}
		memcpy(&a, (char *)chain->in[0].addr + sizeof(req), sizeof(a));

		if (a.nr_entries == 0 || a.nr_entries > 16384) {
			logline("  ATTACH_BACKING res %u: %u entries refused"
				" (limit 16384)\n",
				a.resource_id, a.nr_entries);
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
		cogpu_present_r1_resource_unref(d.resource_id);
		cogpu_present_r2_resource_unref(d.resource_id);
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
			if (type == VIRTIO_GPU_CMD_TRANSFER_FROM_HOST_3D &&
			    present_r1) {
				struct cogpu_vrend_resource_info info;

				if (cogpu_vrend_resource_info(t.resource_id,
							     &info) == 0) {
					int present_rc = cogpu_present_r1_frame(&info,
						t.level, t.x, t.y, t.w, t.h);

					if (present_rc < 0) {
						logline("present R1: disabling after a"
							" context/window failure\n");
						cogpu_present_r1_fini();
						present_r1 = 0;
					}
				}
			}
			resp->type = VIRTIO_GPU_RESP_OK_NODATA;
			g_stats.transfers++;
		} else {
			resp->type = VIRTIO_GPU_RESP_ERR_UNSPEC;
			/* ctx, not just res: virglrenderer checks the context
			 * first, so this line named an innocent resource for
			 * 66,000 lines while the context was the problem. */
			if (refusal_worth_logging(&g_transfer_refusals))
				logline("  TRANSFER res %u ctx %u refused (%d)"
					" [%llu total]\n", t.resource_id,
					req.ctx_id, rc2, g_transfer_refusals);
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

	case VIRTIO_GPU_CMD_RESOURCE_CREATE_BLOB: {
		struct {
			uint32_t resource_id, blob_mem, blob_flags, nr_entries;
			uint64_t blob_id, size;
		} b;
		struct { uint64_t addr; uint32_t length, pad; } ent;
		static struct iovec iov[16384];
		const char *p;
		uint32_t i;
		int bad = 0, niov = 0;

		if (chain->in[0].len < sizeof(req) + sizeof(b)) {
			resp->type = VIRTIO_GPU_RESP_ERR_UNSPEC;
			break;
		}
		memcpy(&b, (char *)chain->in[0].addr + sizeof(req), sizeof(b));

		if (b.nr_entries > 16384) {
			logline("  CREATE_BLOB res %u: %u entries refused"
				" (limit 16384)\n", b.resource_id, b.nr_entries);
			resp->type = VIRTIO_GPU_RESP_ERR_UNSPEC;
			break;
		}

		/* Guest pages gathered exactly as ATTACH_BACKING gathers them. */
		if (b.nr_entries > 0) {
			uint32_t want = b.nr_entries * (uint32_t)sizeof(ent);
			uint32_t got = 0;

			p = chain_gather(chain, 0, sizeof(req) + sizeof(b),
					 want, &got);
			if (!p || got < want) {
				logline("  CREATE_BLOB res %u: %u entries need"
					" %u bytes, chain carries %u\n",
					b.resource_id, b.nr_entries, want, got);
				resp->type = VIRTIO_GPU_RESP_ERR_UNSPEC;
				break;
			}

			for (i = 0; i < b.nr_entries && !bad; i++) {
				uint64_t addr;
				uint32_t left;

				memcpy(&ent, p + i * sizeof(ent), sizeof(ent));
				addr = ent.addr;
				left = ent.length;
				while (left && !bad) {
					uint32_t run = resolve_run(addr, left);
					void *host;

					if (run == 0 ||
					    niov >= (int)(sizeof(iov) / sizeof(iov[0]))) {
						logline("  CREATE_BLOB res %u: entry"
							" %u (0x%llx +%u) refused\n",
							b.resource_id, i,
							(unsigned long long)addr, left);
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
					addr += run;
					left -= run;
				}
			}
			if (bad) {
				resp->type = VIRTIO_GPU_RESP_ERR_UNSPEC;
				break;
			}
		}

		if (cogpu_vrend_create_blob(req.ctx_id, b.resource_id,
					    b.blob_mem, b.blob_flags,
					    b.blob_id, b.size,
					    iov, niov) != 0) {
			logline("  CREATE_BLOB res %u: renderer refused"
				" (mem %u flags 0x%x id %llu size %llu,"
				" %d iovecs)\n",
				b.resource_id, b.blob_mem, b.blob_flags,
				(unsigned long long)b.blob_id,
				(unsigned long long)b.size, niov);
			resp->type = VIRTIO_GPU_RESP_ERR_UNSPEC;
		} else {
			logline("  CREATE_BLOB res %u: mem %u flags 0x%x"
				" size %llu, %d iovecs\n",
				b.resource_id, b.blob_mem, b.blob_flags,
				(unsigned long long)b.size, niov);
			resp->type = VIRTIO_GPU_RESP_OK_NODATA;
		}
		break;
	}

	case VIRTIO_GPU_CMD_RESOURCE_MAP_BLOB: {
		struct { uint32_t resource_id, pad; uint64_t offset; } m;
		struct { uint32_t map_info, pad; } *mi;
		void *va = NULL;
		uint64_t size = 0, mapped_bytes, pseudo_pa;
		uint32_t info = 0;
		co_rc_t rc;

		if (chain->in[0].len < sizeof(req) + sizeof(m) ||
		    chain->out[0].len < sizeof(*resp) + sizeof(*mi)) {
			resp->type = VIRTIO_GPU_RESP_ERR_UNSPEC;
			break;
		}
		memcpy(&m, (char *)chain->in[0].addr + sizeof(req), sizeof(m));

		if (blob_map_find(m.resource_id)) {
			logline("  MAP_BLOB res %u: already mapped\n",
				m.resource_id);
			resp->type = VIRTIO_GPU_RESP_ERR_UNSPEC;
			break;
		}

		if (cogpu_vrend_resource_map(m.resource_id, &va, &size) != 0 ||
		    !va || !size) {
			logline("  MAP_BLOB res %u: renderer would not map\n",
				m.resource_id);
			resp->type = VIRTIO_GPU_RESP_ERR_UNSPEC;
			break;
		}

		/*
		 * The window ioctl is page-granular and the guest maps pages;
		 * a host mapping that does not start on one cannot appear at
		 * the offset the guest chose without shifting the data by the
		 * misalignment. Refused loudly, not worked around quietly --
		 * if a renderer hands these out it needs its own fix.
		 */
		if (((uintptr_t)va & 0xfff) != 0) {
			logline("  MAP_BLOB res %u: host va %p is not page"
				" aligned -- refused\n", m.resource_id, va);
			cogpu_vrend_resource_unmap(m.resource_id);
			resp->type = VIRTIO_GPU_RESP_ERR_UNSPEC;
			break;
		}

		mapped_bytes = (size + 0xfff) & ~0xfffULL;
		pseudo_pa = g_window_base + m.offset;

		if (g_window_top == 0 || pseudo_pa < g_window_base ||
		    pseudo_pa + mapped_bytes > g_window_top) {
			logline("  MAP_BLOB res %u: offset 0x%llx +%llu is"
				" outside the arena\n", m.resource_id,
				(unsigned long long)m.offset,
				(unsigned long long)mapped_bytes);
			cogpu_vrend_resource_unmap(m.resource_id);
			resp->type = VIRTIO_GPU_RESP_ERR_UNSPEC;
			break;
		}

		rc = co_manager_kwindow_at(g_wake_handle, va, mapped_bytes,
					   pseudo_pa);
		if (!CO_OK(rc)) {
			logline("  MAP_BLOB res %u: KWINDOW_AT 0x%llx +%llu"
				" refused, rc %08x\n", m.resource_id,
				(unsigned long long)pseudo_pa,
				(unsigned long long)mapped_bytes, (int)rc);
			cogpu_vrend_resource_unmap(m.resource_id);
			resp->type = VIRTIO_GPU_RESP_ERR_UNSPEC;
			break;
		}

		if (blob_map_track(m.resource_id, pseudo_pa, mapped_bytes,
				   va) != 0) {
			logline("  MAP_BLOB res %u: tracking table full\n",
				m.resource_id);
			co_manager_kunwindow(g_wake_handle, pseudo_pa,
					     mapped_bytes);
			cogpu_vrend_resource_unmap(m.resource_id);
			resp->type = VIRTIO_GPU_RESP_ERR_UNSPEC;
			break;
		}
		cogpu_vrend_map_info(m.resource_id, &info);

		mi = (typeof(mi))((char *)resp + sizeof(*resp));
		mi->map_info = info;
		mi->pad = 0;
		written = sizeof(*resp) + sizeof(*mi);
		resp->type = VIRTIO_GPU_RESP_OK_MAP_INFO;
		logline("  MAP_BLOB res %u: host %p -> guest 0x%llx +%llu,"
			" cache %u\n", m.resource_id, va,
			(unsigned long long)pseudo_pa,
			(unsigned long long)mapped_bytes, info);
		break;
	}

	case VIRTIO_GPU_CMD_MOCO_PRESENT: {
		/*
		 * The Vulkan present: metadata in, pixels stay put. The
		 * blob's memory is mapped for the duration of one texture
		 * upload and released -- vkMapMemory host-side, cheap, and
		 * holding it would pin vkr allocations the guest may free.
		 */
		struct {
			uint32_t resource_id, xid, width, height,
				 stride, flags;
		} mp;
		void *px = NULL;
		uint64_t sz = 0;

		if (chain->in[0].len < sizeof(req) + sizeof(mp)) {
			resp->type = VIRTIO_GPU_RESP_ERR_UNSPEC;
			break;
		}
		memcpy(&mp, (char *)chain->in[0].addr + sizeof(req),
		       sizeof(mp));

		{
			/*
			 * Guest-mapped blobs (the common case: the frame the
			 * guest just wrote through its own mapping) are
			 * already host-mapped by MAP_BLOB, and a second
			 * resource_map on a live blob is refused -- measured,
			 * not assumed. Read through the mapping we hold.
			 */
			struct cogpu_blob_map *bm =
				blob_map_find(mp.resource_id);

			if (bm && bm->host_va) {
				px = bm->host_va;
				sz = bm->bytes;
			} else {
				/*
				 * REFUSED rather than mapped: resource_map
				 * from the serve thread reaches into the vkr
				 * worker's VkDevice cross-thread, and the one
				 * time it ran the worker wedged and the guest
				 * spun forever on its ring. WSI buffers are
				 * guest-mapped at swapchain creation, so a
				 * missing held mapping is itself a bug worth
				 * a logline, not a fallback.
				 */
				logline("  MOCO_PRESENT res %u: no held"
					" mapping -- refused\n",
					mp.resource_id);
				resp->type = VIRTIO_GPU_RESP_ERR_UNSPEC;
				break;
			}

			if (!px || sz < (uint64_t)mp.stride * mp.height) {
				logline("  MOCO_PRESENT res %u: unmappable or"
					" short (%llu < %ux%u)\n",
					mp.resource_id,
					(unsigned long long)sz,
					mp.stride, mp.height);
				resp->type = VIRTIO_GPU_RESP_ERR_UNSPEC;
				break;
			}

			/*
			 * Bridge the Venus frame into a REAL virgl texture
			 * and present through the ONE presenter this project
			 * trusts -- the GL stream path, exactly as gears
			 * uses it, overlay lifecycle and teardown included.
			 * The hand-rolled parallel presenter this replaces
			 * is preserved, as instructed, in
			 * I-AM-A-LITTLE-PIECE-OF-SHIT-FILTH.diff outside the
			 * repo, as a monument.
			 */
			{
				uint32_t res = moco_bridge_resource(mp.xid,
					req.ctx_id, mp.width, mp.height);

				if (!res) {
					logline("  MOCO_PRESENT: bridge"
						" create refused\n");
					resp->type =
						VIRTIO_GPU_RESP_ERR_UNSPEC;
					break;
				}

				if (cogpu_vrend_upload(res, mp.stride,
						       mp.width, mp.height,
						       px, sz) != 0) {
					logline("  MOCO_PRESENT: upload"
						" refused\n");
					resp->type =
						VIRTIO_GPU_RESP_ERR_UNSPEC;
					break;
				}

				cogpu_present_stream(res, mp.xid, mp.width,
						     mp.height, 0, 0,
						     mp.width, mp.height);
				resp->type = VIRTIO_GPU_RESP_OK_NODATA;
			}
		}
		break;
	}

	case VIRTIO_GPU_CMD_RESOURCE_UNMAP_BLOB: {
		struct { uint32_t resource_id, pad; } u;
		struct cogpu_blob_map *bm;

		if (chain->in[0].len < sizeof(req) + sizeof(u)) {
			resp->type = VIRTIO_GPU_RESP_ERR_UNSPEC;
			break;
		}
		memcpy(&u, (char *)chain->in[0].addr + sizeof(req), sizeof(u));

		bm = blob_map_find(u.resource_id);
		if (!bm) {
			logline("  UNMAP_BLOB res %u: not mapped\n",
				u.resource_id);
			resp->type = VIRTIO_GPU_RESP_ERR_UNSPEC;
			break;
		}

		/*
		 * Window first, then renderer. The reverse order frees the
		 * host pages while the guest can still reach them through
		 * the p2m -- the use-after-free the KWINDOW release ordering
		 * exists to prevent.
		 */
		logline("  UNMAP_BLOB res %u: guest 0x%llx +%llu released\n",
			u.resource_id,
			(unsigned long long)bm->pseudo_pa,
			(unsigned long long)bm->bytes);
		co_manager_kunwindow(g_wake_handle, bm->pseudo_pa, bm->bytes);
		cogpu_vrend_resource_unmap(u.resource_id);
		blob_map_forget(u.resource_id);
		resp->type = VIRTIO_GPU_RESP_OK_NODATA;
		break;
	}

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

	/*
	 * Every refusal, named, in one place.
	 *
	 * There are forty places above that can answer ERR_UNSPEC and most of
	 * them say nothing, so the guest logged thousands of
	 * "response 0x1200 (command 0x206)" while this log stayed completely
	 * silent -- the two halves of the same failure, neither of which could
	 * be matched to the other. Whatever refuses, it is visible here with
	 * the command that was refused and the context it was refused for.
	 */
	if (resp->type == VIRTIO_GPU_RESP_ERR_UNSPEC &&
	    refusal_worth_logging(&g_refusals))
		logline("  REFUSED cmd 0x%04x ctx %u [%llu total]\n",
			type, req.ctx_id, g_refusals);

	if (req.flags & VIRTIO_GPU_FLAG_FENCE) {
		resp->flags    = req.flags &
			(VIRTIO_GPU_FLAG_FENCE | VIRTIO_GPU_FLAG_INFO_RING_IDX);
		resp->fence_id = req.fence_id;
		resp->ctx_id   = req.ctx_id;
		resp->ring_idx = req.ring_idx;
		g_stats.fenced++;

		/*
		 * GL completes inline because virgl_renderer_submit_cmd is
		 * synchronous -- by the time this runs, the work is done, and
		 * that path stays exactly as it has been since R4. Venus is
		 * not: vkr executes on its own ring threads, and the virtio
		 * fence contract is that THIS RESPONSE does not appear in the
		 * used ring until the fence retires. So a real per-context
		 * fence is created and the loop parks the chain; the fence
		 * callback releases it. An error response is not parked --
		 * nothing was submitted, so nothing will ever signal it.
		 */
		if (ctx_is_venus(req.ctx_id) &&
		    resp->type != VIRTIO_GPU_RESP_ERR_UNSPEC) {
			uint32_t ring =
				(req.flags & VIRTIO_GPU_FLAG_INFO_RING_IDX)
					? req.ring_idx : 0;

			if (cogpu_vrend_ctx_fence(req.ctx_id, ring,
						  req.fence_id) == 0) {
				g_park_req.pending  = 1;
				g_park_req.ctx_id   = req.ctx_id;
				g_park_req.ring_idx = ring;
				g_park_req.fence_id = req.fence_id;
			} else {
				logline("  ctx %u ring %u: fence %llu refused"
					" by renderer -- completing inline\n",
					req.ctx_id, ring,
					(unsigned long long)req.fence_id);
			}
		}
	}

	g_stats.requests++;
	g_stats.by_type[(type >> 8) & 0xf]++;

	if (verbose)
		logline("  req type 0x%04x%s -> 0x%04x\n", type,
		       (req.flags & VIRTIO_GPU_FLAG_FENCE) ? " (fenced)" : "",
		       resp->type);

	return written;
}

/* ---------------------------------------------------------- kwindow test */

/*
 * The pattern round-trip that must pass before any Vulkan work is credible:
 * this process's memory, appearing in the guest's physical address space,
 * readable and writable from both sides at once.
 *
 * Protocol, driven by hand from a guest shell (busybox devmem):
 *
 *   devmem 0x<pa>          64                       -> 0xC0DE000000000000
 *   devmem 0x<pa+8>        64                       -> 0xC0DE000000000001
 *   devmem 0x<pa+0x1000>   64 0xAA55AA55AA55AA55
 *   devmem 0x<pa+0x1FFFF8> 64 0x1122334455667788
 *   devmem 0x<pa>          64 0x600DBEEFCAFEF00D    <- magic last
 *
 * The magic is written last so that when it appears here, the other two
 * writes are already resident; three probes cover first page, an interior
 * page, and the final eight bytes of the window.
 */
static int cogpu_kwindow_test(co_manager_handle_t handle)
{
	enum { KWT_BYTES = 2 << 20 };
	static const unsigned long long KWT_MAGIC = 0x600DBEEFCAFEF00Dull;
	volatile unsigned long long *buf;
	volatile unsigned long long *at_buf;
	unsigned long long pseudo_pa = 0, at_base;
	unsigned long long i, n = KWT_BYTES / 8;
	int waited, failed = 0;
	co_rc_t rc;

	buf = VirtualAlloc(NULL, KWT_BYTES, MEM_COMMIT | MEM_RESERVE,
			   PAGE_READWRITE);
	if (!buf) {
		logline("kwindow-test: VirtualAlloc failed (%lu)\n",
			GetLastError());
		return 1;
	}

	for (i = 0; i < n; i++)
		buf[i] = 0xC0DE000000000000ull | i;

	rc = co_manager_kwindow(handle, (const void *)buf, KWT_BYTES,
				&pseudo_pa);
	if (!CO_OK(rc)) {
		logline("kwindow-test: KWINDOW refused, rc %08x\n", (int)rc);
		return 1;
	}

	logline("kwindow-test: %u KB of this process at guest pa 0x%llx\n",
		KWT_BYTES >> 10, pseudo_pa);

	/*
	 * The AT variant, on the same round trip: a page placed at the arena
	 * base -- the address MAP_BLOB uses for offset 0. Placement and
	 * vacancy refusal are checked here; the guest writes a token through
	 * it before the magic, verified after. New driver code, one probe.
	 */
	at_buf = NULL;
	at_base = 0;
	{
		unsigned long long top = 0;

		if (CO_OK(co_manager_window_bounds(handle, &at_base, &top)) &&
		    top > at_base) {
			at_buf = VirtualAlloc(NULL, 0x1000,
					      MEM_COMMIT | MEM_RESERVE,
					      PAGE_READWRITE);
		}
		if (at_buf) {
			rc = co_manager_kwindow_at(handle, (void *)at_buf,
						   0x1000, at_base);
			if (!CO_OK(rc)) {
				logline("kwindow-test: FAIL -- AT window at"
					" arena base 0x%llx refused,"
					" rc %08x\n", at_base, (int)rc);
				at_buf = NULL;
				failed = 1;
			} else if (CO_OK(co_manager_kwindow_at(handle,
							       (void *)at_buf,
							       0x1000,
							       at_base))) {
				logline("kwindow-test: FAIL -- AT vacancy"
					" check: occupied slot re-mapped\n");
				failed = 1;
			}
		} else {
			logline("kwindow-test: AT variant skipped (no arena"
				" or no memory)\n");
		}
	}

	logline("kwindow-test: from a guest shell, in this order:\n");
	logline("  devmem 0x%llx 64                       # expect 0xc0de000000000000\n",
		pseudo_pa);
	logline("  devmem 0x%llx 64                       # expect 0xc0de000000000001\n",
		pseudo_pa + 8);
	logline("  devmem 0x%llx 64 0xAA55AA55AA55AA55\n", pseudo_pa + 0x1000);
	logline("  devmem 0x%llx 64 0x1122334455667788\n",
		pseudo_pa + KWT_BYTES - 8);
	if (at_buf)
		logline("  devmem 0x%llx 64 0x5A5A5A5A5A5A5A5A\n", at_base);
	logline("  devmem 0x%llx 64 0x600DBEEFCAFEF00D\n", pseudo_pa);
	logline("kwindow-test: waiting up to 10 minutes for the magic...\n");

	for (waited = 0; waited < 10 * 60 * 5; waited++) {
		if (buf[0] == KWT_MAGIC)
			break;
		Sleep(200);
	}

	if (buf[0] != KWT_MAGIC) {
		logline("kwindow-test: FAIL -- magic never arrived;"
			" +0x0 still 0x%llx\n", buf[0]);
		co_manager_kunwindow(handle, pseudo_pa, KWT_BYTES);
		return 1;
	}

	/* The AT window's guest write must be resident by now too -- the
	 * protocol orders it before the magic. */
	if (at_buf) {
		if (at_buf[0] != 0x5A5A5A5A5A5A5A5Aull) {
			logline("kwindow-test: FAIL -- AT window still 0x%llx,"
				" wanted 0x5a5a5a5a5a5a5a5a\n", at_buf[0]);
			failed = 1;
		} else {
			logline("kwindow-test: AT window round-trip OK\n");
		}
		co_manager_kunwindow(handle, at_base, 0x1000);
	}

	if (buf[0x1000 / 8] != 0xAA55AA55AA55AA55ull) {
		logline("kwindow-test: FAIL -- +0x1000 is 0x%llx, wanted"
			" 0xaa55aa55aa55aa55\n", buf[0x1000 / 8]);
		failed = 1;
	}
	if (buf[n - 1] != 0x1122334455667788ull) {
		logline("kwindow-test: FAIL -- last u64 is 0x%llx, wanted"
			" 0x1122334455667788\n", buf[n - 1]);
		failed = 1;
	}
	/* Slots the guest never touched must still hold the pattern. */
	for (i = 0; i < n; i++) {
		if (i == 0 || i == 0x1000 / 8 || i == n - 1)
			continue;
		if (buf[i] != (0xC0DE000000000000ull | i)) {
			logline("kwindow-test: FAIL -- untouched +0x%llx"
				" corrupted to 0x%llx\n", i * 8, buf[i]);
			failed = 1;
			break;
		}
	}

	rc = co_manager_kunwindow(handle, pseudo_pa, KWT_BYTES);
	if (!CO_OK(rc)) {
		logline("kwindow-test: KUNWINDOW refused, rc %08x\n", (int)rc);
		failed = 1;
	}

	logline("kwindow-test: %s\n", failed ? "FAIL" : "PASS");
	return failed;
}

/* ------------------------------------------------------------------- main */

int main(int argc, char **argv)
{
	co_manager_handle_t	handle;
	struct co_vgpu_io_abi  *io = NULL;
	struct cogpu_vring	vq[CO_VGPU_NUM_VQS];
	unsigned int		last_kick[CO_VGPU_NUM_VQS] = { 0, 0 };
	unsigned long long	vgpu_va = 0;
	int			i, once = 0, present_probe = 0;
	int			kwindow_test = 0;
	int			idle = 0;
	unsigned long long	sweeps = 0;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--verbose"))
			verbose = 1;
		else if (!strcmp(argv[i], "--once"))
			once = 1;
		else if (!strcmp(argv[i], "--present-probe"))
			present_probe = 1;
		else if (!strcmp(argv[i], "--present-r1"))
			present_r1 = 1;
		else if (!strcmp(argv[i], "--present-r2"))
			present_r2 = 1;
		else if (!strcmp(argv[i], "--kwindow-test"))
			kwindow_test = 1;
		else if (!strcmp(argv[i], "--selftest"))
			return cogpu_vring_selftest();
		else {
			logline("usage: cogpu-daemon [--verbose] [--once] [--selftest]"
				" [--present-probe] [--present-r1] [--present-r2]"
				" [--kwindow-test]\n");
			return 2;
		}
	}
	if (present_r1 && present_r2) {
		logline("--present-r1 and --present-r2 are mutually exclusive\n");
		return 2;
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
		/*
		 * A test instance runs beside the live service daemon, which
		 * holds the real log open -- "w" here would truncate it under
		 * the service's feet. Tests get their own file.
		 */
		const char *base = kwindow_test ? "cogpu-kwindow-test.log"
						: "cogpu-daemon.log";

		if (n > 0 && n < sizeof(drive))
			_snprintf(path, sizeof(path), "%s\\MoCoLinux\\%s",
				  drive, base);
		else
			_snprintf(path, sizeof(path), "%s", base);
		path[sizeof(path) - 1] = '\0';

		g_log = fopen(path, "w");
		if (!g_log)
			g_log = fopen(base, "w");
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
	if (!kwindow_test) {
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

	InitializeCriticalSection(&g_fence_lock);
	{
		unsigned i;

		for (i = 0; i < sizeof(g_parked) / sizeof(g_parked[0]); i++)
			g_parked[i].vq = -1;
		g_parked_init_done = 1;
	}
	cogpu_vrend_set_ctx_fence_cb(cogpu_ctx_fence_signal);

	/*
	 * The ICD, for a box where it is not registered. This machine's
	 * NVIDIA install never wrote HKLM\SOFTWARE\Khronos\Vulkan\Drivers,
	 * so the loader enumerates zero devices unless told per-process.
	 * Only when unset, only if the json actually exists -- on any other
	 * machine this is a no-op and the registry does its normal job.
	 */
	if (GetEnvironmentVariableA("VK_ICD_FILENAMES", NULL, 0) == 0) {
		const char *icd = "C:\\Windows\\System32\\DriverStore\\"
			"FileRepository\\nvhdc.inf_amd64_3142914239be99ea\\"
			"nv-vk64.json";

		if (GetFileAttributesA(icd) != INVALID_FILE_ATTRIBUTES) {
			SetEnvironmentVariableA("VK_ICD_FILENAMES", icd);
			logline("VK_ICD_FILENAMES set to the unregistered"
				" NVIDIA ICD\n");
		}
	}

	/*
	 * vkr's own diagnostics, into cogpu-vrend.log. Cheap when nothing
	 * fails; the only voice in the room when a venus device refuses to
	 * create and every layer above shows a bare -3.
	 */
	if (GetEnvironmentVariableA("VKR_DEBUG", NULL, 0) == 0)
		SetEnvironmentVariableA("VKR_DEBUG", "all");

	/*
	 * The renderer, before anything else that matters. If it will not come
	 * up there is no point servicing a ring: the guest would get OK to
	 * everything and render nothing, which is worse than a device that
	 * refuses honestly.
	 *
	 * The kwindow test is the one exception: it exercises the driver's
	 * window path and nothing else, so a broken renderer must not be able
	 * to block it.
	 */
	if (!kwindow_test && cogpu_vrend_init(NULL, NULL) != 0) {
		logline("virglrenderer would not initialise -- no GPU today\n");
		return 1;
	}
	if (!kwindow_test) {
		logline("renderer: %s\n", cogpu_vrend_renderer());
		logline("venus: %s\n", cogpu_vrend_has_venus()
			? "up -- Vulkan forwarding available"
			: "not available -- GL only (no vulkan-1.dll or no"
			  " ICD); everything still works");
		{
			unsigned int v = 0, sz = 0;

			cogpu_vrend_capset(1, &v, &sz);
			logline("virgl capset: version %u, %u bytes\n", v, sz);
			if (cogpu_vrend_has_venus()) {
				cogpu_vrend_capset(4, &v, &sz);
				logline("venus capset: version %u, %u bytes\n",
					v, sz);
			}
		}
		if (present_probe)
			return cogpu_present_probe(12000);
		if (present_r1)
			cogpu_present_r1_enable();
		if (present_r2) {
			cogpu_present_r2_enable();
			/* Presentation requests carried in the guest's own
			 * command stream, which is what makes sandboxed clients
			 * work with no configuration -- see
			 * cogpu_present_stream(). */
			cogpu_vrend_set_present_hook(cogpu_present_stream);
		}
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
	 * The window arena, asked of the driver once. Failure leaves both at
	 * zero, and MAP_BLOB then refuses -- an old driver degrades to the
	 * pre-blob behaviour instead of mapping things at address zero.
	 */
	if (CO_OK(co_manager_window_bounds(handle, &g_window_base,
					   &g_window_top)))
		logline("window arena: 0x%llx..0x%llx (%llu MB)\n",
			g_window_base, g_window_top,
			(g_window_top - g_window_base) >> 20);
	else
		logline("window arena unavailable (old driver?);"
			" MAP_BLOB will refuse\n");

	/*
	 * The verdict on the precise-sleep machinery, in a log file where it
	 * can be read: zero means the idle wait is blind and every guest
	 * sleep costs a full tick regardless of what the guest asked for.
	 */
	{
		co_manager_ioctl_vgpu_t vp = {0, };
		unsigned long returned = 0;

		if (CO_OK(co_os_manager_ioctl(handle, CO_MANAGER_IOCTL_VGPU,
					      &vp, sizeof(vp), &vp, sizeof(vp),
					      &returned)) && CO_OK(vp.rc))
			logline("deadline machinery (loop truth): ptr %llx,"
				" branch %llu, spins %llu, hires-timer %s,"
				" hires-selftest %llu00ns | paths: tick %llu"
				" far %llu mid %llu due %llu\n",
				vp.timer_deadline_host,
				vp.tdl_branch & 0xffffffffULL,
				vp.tdl_spins & 0x7fffffffffffffffULL,
				(vp.tdl_spins >> 63) ? "YES" : "NO",
				(vp.tdl_branch >> 48) & 0xffffULL,
				vp.tdl_paths_a >> 32,
				vp.tdl_paths_a & 0xffffffffULL,
				vp.tdl_paths_b >> 32,
				vp.tdl_paths_b & 0xffffffffULL);
	}

	if (kwindow_test) {
		int r = cogpu_kwindow_test(handle);

		co_os_manager_close(handle);
		return r;
	}

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
	if (present_r2 && cogpu_copresent_start() != 0) {
		logline("present R2: metadata listener unavailable; disabling R2\n");
		cogpu_present_r2_fini();
		present_r2 = 0;
	}

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
		int present_poll_rc;
		int copresent_poll_rc;

		present_poll_rc = present_r1 ? cogpu_present_r1_poll() :
			(present_r2 ? cogpu_present_r2_poll() : 0);
		if (present_poll_rc < 0) {
			logline("present: disabling after an overlay/message-pump"
				" failure\n");
			if (present_r1) {
				cogpu_present_r1_fini();
				present_r1 = 0;
			} else {
				cogpu_copresent_fini();
				cogpu_present_r2_fini();
				present_r2 = 0;
			}
		}
		copresent_poll_rc = present_r2 ? cogpu_copresent_poll() : 0;
		if (copresent_poll_rc < 0) {
			logline("present R2: metadata channel failed; disabling R2\n");
			cogpu_copresent_fini();
			cogpu_present_r2_fini();
			present_r2 = 0;
		}

		io->host_heartbeat++;
		sweeps++;

		if (GetTickCount() - last > 2000) {
			last = GetTickCount();
			/* submits/cmd_bytes ride along so per-frame command volume
			 * is a subtraction between two lines, not an exit report the
			 * teardown kill never lets print. */
			logline("[%lus] status 0x%x q0 num %u kick %u  q1 num %u kick %u"
			       "  served %llu  submits %llu  cmd %llu\n",
			       (unsigned long)((last - t0) / 1000),
			       io->status,
			       io->vq[0].num, io->vq[0].kick,
			       io->vq[1].num, io->vq[1].kick,
			       g_stats.requests, g_stats.submits, g_stats.cmd_bytes);
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

				/*
				 * A fenced Venus request is answered but not
				 * published: the used element appears when
				 * the fence retires, which is the virtio
				 * fence contract. A full park table falls
				 * back to inline completion -- wrong ordering
				 * is survivable, a lost request is not.
				 */
				if (g_park_req.pending) {
					g_park_req.pending = 0;
					if (cogpu_park(i, chain.head, written,
						       g_park_req.ctx_id,
						       g_park_req.ring_idx,
						       g_park_req.fence_id) == 0)
						continue;
					logline("park table full; completing"
						" ctx %u fence %llu inline\n",
						g_park_req.ctx_id,
						(unsigned long long)
						g_park_req.fence_id);
				}
				cogpu_vring_push(&vq[i], &chain, written);
				did = 1;
			}
		}

		/*
		 * Fences that retired on vkr's threads since the last pass.
		 * Publishing happens HERE, on the thread that owns the
		 * vrings; the callback only queued the triple. One signal
		 * releases every parked request on that ring up to the id,
		 * because per-ring fences retire in order.
		 */
		if (g_fence_sig_head != g_fence_sig_tail) {
			EnterCriticalSection(&g_fence_lock);
			while (g_fence_sig_tail != g_fence_sig_head) {
				unsigned slot = g_fence_sig_tail++ %
					(sizeof(g_fence_sig) /
					 sizeof(g_fence_sig[0]));
				uint32_t ctx  = g_fence_sig[slot].ctx_id;
				uint32_t ring = g_fence_sig[slot].ring_idx;
				uint64_t id   = g_fence_sig[slot].fence_id;
				unsigned p;

				for (p = 0; p < sizeof(g_parked) /
					    sizeof(g_parked[0]); p++) {
					struct cogpu_parked *pk = &g_parked[p];
					struct cogpu_chain stub;

					if (pk->vq < 0 || pk->ctx_id != ctx ||
					    pk->ring_idx != ring ||
					    pk->fence_id > id)
						continue;
					stub.head = pk->head;
					cogpu_vring_push(&vq[pk->vq], &stub,
							 pk->written);
					pk->vq = -1;
					did = 1;
				}
			}
			LeaveCriticalSection(&g_fence_lock);
		}
		if (g_fence_sig_dropped) {
			logline("WARNING: %ld fence signals dropped -- guest"
				" fences may hang; enlarge g_fence_sig\n",
				(long)g_fence_sig_dropped);
			g_fence_sig_dropped = 0;
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

	cogpu_copresent_fini();
	cogpu_present_r2_fini();
	cogpu_present_r1_fini();

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
