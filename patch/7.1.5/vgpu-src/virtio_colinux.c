// SPDX-License-Identifier: GPL-2.0
/*
 * virtio over a cooperative-Linux ring, with no registers and no traps.
 *
 * The guest already contains everything needed to drive a GPU: the virtio-gpu
 * driver is compiled in, Mesa ships the virgl Gallium driver, and the vring
 * code is stock. What has always been missing is a TRANSPORT -- something to
 * hand virtio a device with ID 16 -- because this guest runs with pci=off and
 * there is nowhere for a PCI device to come from.
 *
 * virtio_mmio is the obvious model and it does not work here either, for a
 * reason worth stating plainly: MMIO transports work because a guest store to
 * a fake register TRAPS, and the hypervisor sees it. There is no hypervisor
 * here. Both kernels run on the bare processor, a store to any address is just
 * a store, and nobody is notified. So this transport is virtio_mmio with every
 * register access replaced by a plain memory write into a structure the host
 * can already see -- which it can, because guest physical memory IS host
 * physical memory in this design.
 *
 * That single property is what makes the whole thing cheap:
 *
 *   - The "kick" is an increment of a counter in shared memory. The host
 *     daemon spin-polls it on another core while the guest is still running.
 *     No crossing, no exit, no notification path at all.
 *
 *   - The vrings need no translation. A transport that does not offer
 *     VIRTIO_F_ACCESS_PLATFORM makes the vring code bypass the DMA API
 *     entirely, so every descriptor address is a guest physical address --
 *     and the host can dereference those directly through the persistent
 *     mappings R3 established. Vertex data and textures are never marshalled;
 *     only the command dwords cross anything.
 *
 *   - Completions arrive the way every other asynchronous path in this port
 *     works: the host writes the used ring, and the guest calls
 *     vring_interrupt() from the existing cooperative drain points. The
 *     waiting thread SLEEPS in stock dma_fence code and is woken there. It
 *     must never spin: this guest has one processor, and a thread spinning on
 *     a fence stops the entire machine, which is the mistake the block path
 *     was built to avoid.
 *
 * num_scanouts is zero on purpose. This is a render-only device: presentation
 * belongs to the X server on the Windows side, which already owns a real
 * window per guest X window. The virtio-gpu driver has a KMS-disabled path for
 * exactly this case and takes it when the config says there are no scanouts.
 */

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/virtio.h>
#include <linux/virtio_config.h>
#include <linux/virtio_ring.h>
#include <linux/virtio_ids.h>
#include <linux/platform_device.h>
#include <linux/delay.h>
#include <linux/kthread.h>
#include <linux/console.h>	/* co_vgpu_drain's prototype lives with the
					 * other cooperative hooks */

/*
 * The shared structure, and the one rule about it: this layout is ABI.
 *
 * The host has its own copy of this declaration in
 * src/colinux/kernel/vgpu.h. There is deliberately no shared header -- the two
 * sides are built by different toolchains for different kernels -- so the
 * contract is enforced by the BUILD_BUG_ON block at the bottom of this file
 * and a matching set of compile-time assertions on the host. If you change a
 * field here, both sets of assertions must be updated together or one side
 * will fail to build, which is the intended outcome.
 *
 * Every field is fixed-width for the same reason.
 */
struct co_vgpu_vq {
	__u64 desc_gpa;		/* guest physical == host physical */
	__u64 avail_gpa;
	__u64 used_gpa;
	__u32 num;
	__u32 kick;		/* guest increments; the daemon polls it */
};

struct co_vgpu_io {
	__u32 magic;		/* 'VGPU'; the boot daemon writes it */
	__u32 abi_version;
	__u64 host_features;
	__u64 guest_features;
	__u32 status;		/* the virtio status byte, guest-owned */
	__u32 enabled;		/* daemon presence; cleared on clean exit */
	__u32 num_capsets;
	__u32 num_scanouts;	/* zero: render-only, no KMS */
	struct co_vgpu_vq vq[2];	/* controlq, cursorq */
	__u32 used_pending;	/* host bumps when it publishes completions */
	__u32 reaped;		/* guest bumps when it has consumed them */
	__u32 guest_heartbeat;
	__u32 host_heartbeat;
};

#define CO_VGPU_MAGIC		0x55504756	/* 'VGPU' little-endian */
#define CO_VGPU_ABI_VERSION	1
#define CO_VGPU_NUM_VQS		2

/*
 * In .bss, resolved by name from the host's ELF loader, exactly like
 * co_colinux_console_ring and co_colinux_net_io before it. The host writes the
 * magic and the feature bits into it between loading the image and starting
 * the guest, so by the time this code runs the structure is either populated
 * or plainly absent -- and absent means "no GPU today", not a failure.
 */
struct co_vgpu_io co_colinux_vgpu_io;
EXPORT_SYMBOL(co_colinux_vgpu_io);

struct co_vgpu_device {
	struct virtio_device	vdev;
	struct platform_device *pdev;
	struct virtqueue       *vqs[CO_VGPU_NUM_VQS];
	spinlock_t		lock;
};

static struct co_vgpu_device *co_vgpu;

static struct co_vgpu_device *to_co_vgpu(struct virtio_device *vdev)
{
	return container_of(vdev, struct co_vgpu_device, vdev);
}

/* ------------------------------------------------------------ config space */

/*
 * The device's configuration space is four little-endian words, and all four
 * are constants here: no events are ever raised, there are no scanouts, and
 * the capset count is whatever the host told us it can render.
 */
static void co_vgpu_get(struct virtio_device *vdev, unsigned int offset,
			void *buf, unsigned int len)
{
	struct virtio_gpu_config {
		__le32 events_read;
		__le32 events_clear;
		__le32 num_scanouts;
		__le32 num_capsets;
	} cfg;

	cfg.events_read  = cpu_to_le32(0);
	cfg.events_clear = cpu_to_le32(0);
	cfg.num_scanouts = cpu_to_le32(co_colinux_vgpu_io.num_scanouts);
	cfg.num_capsets  = cpu_to_le32(co_colinux_vgpu_io.num_capsets);

	if (offset >= sizeof(cfg))
		return;
	if (len > sizeof(cfg) - offset)
		len = sizeof(cfg) - offset;

	memcpy(buf, (char *)&cfg + offset, len);
}

static void co_vgpu_set(struct virtio_device *vdev, unsigned int offset,
			const void *buf, unsigned int len)
{
	/* events_clear is the only writable field and there are no events. */
}

static u8 co_vgpu_get_status(struct virtio_device *vdev)
{
	return (u8)co_colinux_vgpu_io.status;
}

/*
 * Publishing status is a store, and then we carry on.
 *
 * A register-based transport would write a register and the device would
 * react synchronously. Here the daemon notices in its own time. Waiting for an
 * acknowledgement would mean spinning on the guest's only processor during
 * probe, so this deliberately does not: the ordering that matters (rings
 * published before DRIVER_OK) is guaranteed by the store order below, not by a
 * handshake.
 */
static void co_vgpu_set_status(struct virtio_device *vdev, u8 status)
{
	smp_wmb();
	WRITE_ONCE(co_colinux_vgpu_io.status, status);
}

static void co_vgpu_reset(struct virtio_device *vdev)
{
	WRITE_ONCE(co_colinux_vgpu_io.status, 0);
}

static u64 co_vgpu_get_features(struct virtio_device *vdev)
{
	/*
	 * Whatever the host offered, minus anything this transport cannot
	 * honour. VIRTIO_F_ACCESS_PLATFORM is masked off unconditionally and
	 * that is not an oversight: offering it would put the vring behind the
	 * DMA API, descriptors would carry DMA addresses instead of guest
	 * physical ones, and the host would have to translate every one. Its
	 * absence is what makes resource memory readable in place.
	 */
	u64 f = co_colinux_vgpu_io.host_features;

	f &= ~(1ULL << VIRTIO_F_ACCESS_PLATFORM);
	return f;
}

static int co_vgpu_finalize_features(struct virtio_device *vdev)
{
	if (!__virtio_test_bit(vdev, VIRTIO_F_VERSION_1)) {
		dev_err(&vdev->dev, "colinux vgpu: host must offer VERSION_1\n");
		return -EINVAL;
	}

	if (__virtio_test_bit(vdev, VIRTIO_F_ACCESS_PLATFORM)) {
		dev_err(&vdev->dev, "colinux vgpu: ACCESS_PLATFORM negotiated,"
				    " which this transport cannot support\n");
		return -EINVAL;
	}

	WRITE_ONCE(co_colinux_vgpu_io.guest_features, vdev->features);
	return 0;
}

/* ---------------------------------------------------------------- the kick */

/*
 * The entire notification mechanism.
 *
 * A store. The daemon is spinning on this word on another physical core, sees
 * the increment by cache coherency, and starts work -- while this guest thread
 * carries straight on. There is no crossing here and no host entry: that is
 * the difference between this and every hypervisor transport, and it is why
 * submission costs nothing measurable.
 */
static bool co_vgpu_notify(struct virtqueue *vq)
{
	struct co_vgpu_vq *shared = &co_colinux_vgpu_io.vq[vq->index];

	/* Descriptors and the avail index must be visible before the kick that
	 * advertises them. */
	smp_wmb();
	WRITE_ONCE(shared->kick, READ_ONCE(shared->kick) + 1);
	return true;
}

/* ------------------------------------------------------------- virtqueues */

static void co_vgpu_del_vqs(struct virtio_device *vdev)
{
	struct co_vgpu_device *cd = to_co_vgpu(vdev);
	struct virtqueue *vq, *n;
	int i;

	list_for_each_entry_safe(vq, n, &vdev->vqs, list)
		vring_del_virtqueue(vq);

	for (i = 0; i < CO_VGPU_NUM_VQS; i++) {
		cd->vqs[i] = NULL;
		memset(&co_colinux_vgpu_io.vq[i], 0,
		       sizeof(co_colinux_vgpu_io.vq[i]));
	}
}

static int co_vgpu_find_vqs(struct virtio_device *vdev, unsigned int nvqs,
			    struct virtqueue *vqs[],
			    struct virtqueue_info vqs_info[],
			    struct irq_affinity *desc)
{
	struct co_vgpu_device *cd = to_co_vgpu(vdev);
	unsigned int i;

	if (nvqs > CO_VGPU_NUM_VQS)
		return -EINVAL;

	for (i = 0; i < nvqs; i++) {
		struct virtqueue_info *vqi = &vqs_info[i];
		struct virtqueue *vq;

		if (!vqi->name) {
			vqs[i] = NULL;
			continue;
		}

		/*
		 * 256 descriptors, page-aligned. The ring is ordinary guest
		 * memory; the host reads it in place, so its size is a
		 * queue-depth decision and nothing else.
		 */
		vq = vring_create_virtqueue(i, 256, PAGE_SIZE, vdev,
					    true,	/* weak barriers: same
							 * machine, real
							 * coherency */
					    true,	/* may reduce num */
					    vqi->ctx,
					    co_vgpu_notify,
					    vqi->callback,
					    vqi->name);
		if (!vq) {
			co_vgpu_del_vqs(vdev);
			return -ENOMEM;
		}

		/*
		 * Publish where the host can find it. These are guest physical
		 * addresses precisely because ACCESS_PLATFORM was refused
		 * above; with it, they would be DMA addresses and meaningless
		 * to the daemon.
		 */
		co_colinux_vgpu_io.vq[i].desc_gpa  = virtqueue_get_desc_addr(vq);
		co_colinux_vgpu_io.vq[i].avail_gpa = virtqueue_get_avail_addr(vq);
		co_colinux_vgpu_io.vq[i].used_gpa  = virtqueue_get_used_addr(vq);
		co_colinux_vgpu_io.vq[i].num	   = virtqueue_get_vring_size(vq);
		co_colinux_vgpu_io.vq[i].kick	   = 0;

		cd->vqs[i] = vq;
		vqs[i]	   = vq;
	}

	/* The rings must be visible before anything can be told they exist. */
	smp_wmb();
	return 0;
}

static const char *co_vgpu_bus_name(struct virtio_device *vdev)
{
	return "colinux";
}

static const struct virtio_config_ops co_vgpu_config_ops = {
	.get			= co_vgpu_get,
	.set			= co_vgpu_set,
	.get_status		= co_vgpu_get_status,
	.set_status		= co_vgpu_set_status,
	.reset			= co_vgpu_reset,
	.find_vqs		= co_vgpu_find_vqs,
	.del_vqs		= co_vgpu_del_vqs,
	.get_features		= co_vgpu_get_features,
	.finalize_features	= co_vgpu_finalize_features,
	.bus_name		= co_vgpu_bus_name,
};

/* ------------------------------------------------------------ completions */

/*
 * Called from the cooperative drain points, not from an interrupt.
 *
 * The host has written the used ring and bumped used_pending. This runs the
 * stock virtio completion path, which retires fences and wakes whatever was
 * sleeping on them. It is deliberately reachable from the timer tick, the idle
 * drain and the exit-to-user path -- the same three places the block driver's
 * completions are reaped -- because a completion that only arrives on one of
 * them is a completion that can be delayed arbitrarily by a guest that happens
 * not to take that path.
 */
void co_vgpu_drain(void)
{
	u32 pending;
	int i;

	if (!co_vgpu)
		return;

	pending = READ_ONCE(co_colinux_vgpu_io.used_pending);
	WRITE_ONCE(co_colinux_vgpu_io.guest_heartbeat,
		   READ_ONCE(co_colinux_vgpu_io.guest_heartbeat) + 1);

	if (pending == READ_ONCE(co_colinux_vgpu_io.reaped))
		return;

	WRITE_ONCE(co_colinux_vgpu_io.reaped, pending);
	smp_rmb();

	for (i = 0; i < CO_VGPU_NUM_VQS; i++) {
		if (co_vgpu->vqs[i])
			vring_interrupt(0, co_vgpu->vqs[i]);
	}
}
EXPORT_SYMBOL(co_vgpu_drain);

/* ------------------------------------------------------------------- probe */

static void co_vgpu_release(struct device *dev)
{
	/* The virtio device is embedded in a static allocation. */
}

/*
 * Poll for the daemon, then publish the device.
 *
 * Two minutes is generous on purpose: the daemon maps a gigabyte of guest RAM
 * before it sets this flag, and on a fragmented host that is not instant. A
 * guest that waits a little is strictly better than one that decides too early
 * that it has no GPU.
 */
static int co_vgpu_wait_and_register(void *arg)
{
	struct co_vgpu_device *cd = arg;
	int waited;

	for (waited = 0; waited < 120000; waited += 100) {
		if (kthread_should_stop())
			return 0;
		if (READ_ONCE(co_colinux_vgpu_io.enabled))
			break;
		msleep(100);
	}

	if (!READ_ONCE(co_colinux_vgpu_io.enabled)) {
		pr_info("colinux vgpu: no host daemon appeared; no GPU this run\n");
		return 0;
	}

	if (register_virtio_device(&cd->vdev)) {
		pr_err("colinux vgpu: register_virtio_device failed\n");
		return 0;
	}

	pr_info("colinux vgpu: host daemon is serving; device registered,"
		" %u capsets\n", co_colinux_vgpu_io.num_capsets);
	return 0;
}

static int __init co_vgpu_init(void)
{
	struct co_vgpu_device *cd;
	int rc;

	/*
	 * No magic means the host did not set this up: either an older daemon,
	 * or a run with no GPU daemon at all. That is a normal configuration,
	 * so it is one quiet line rather than an error.
	 */
	if (READ_ONCE(co_colinux_vgpu_io.magic) != CO_VGPU_MAGIC) {
		pr_info("colinux vgpu: no host device offered\n");
		return 0;
	}

	if (co_colinux_vgpu_io.abi_version != CO_VGPU_ABI_VERSION) {
		pr_err("colinux vgpu: ABI %u, expected %u -- refusing\n",
		       co_colinux_vgpu_io.abi_version, CO_VGPU_ABI_VERSION);
		return -ENODEV;
	}

	cd = kzalloc(sizeof(*cd), GFP_KERNEL);
	if (!cd)
		return -ENOMEM;

	spin_lock_init(&cd->lock);

	/*
	 * A parent device, and it is not a formality.
	 *
	 * virtio_gpu_probe hands vdev->dev.parent straight to drm_dev_alloc,
	 * and DRM refuses a NULL parent with -EINVAL -- which is exactly how
	 * this failed the first time it ran: the transport registered, the
	 * driver bound, and probe returned -22 before printing a single line
	 * of its own, because the failure is upstream of everything that
	 * reports. A real transport gets this for free (virtio_mmio has its
	 * platform device, virtio_pci its PCI device); a transport invented
	 * from nothing has to supply one.
	 *
	 * A bare platform device is the honest choice. It is not pretending to
	 * be hardware -- dev_is_pci() on it is false, so virtio_gpu skips its
	 * PCI quirk -- it exists to give DRM something to hang the device on.
	 */
	cd->pdev = platform_device_register_simple("colinux-vgpu", -1, NULL, 0);
	if (IS_ERR(cd->pdev)) {
		int rc2 = PTR_ERR(cd->pdev);

		pr_err("colinux vgpu: no parent device (%d)\n", rc2);
		kfree(cd);
		return rc2;
	}

	cd->vdev.id.device = VIRTIO_ID_GPU;
	cd->vdev.id.vendor = 0x434f4c58;	/* 'COLX' */
	cd->vdev.config    = &co_vgpu_config_ops;
	cd->vdev.dev.parent  = &cd->pdev->dev;
	cd->vdev.dev.release = co_vgpu_release;

	co_vgpu = cd;

	/*
	 * Wait for the host daemon before letting virtio-gpu see the device.
	 *
	 * The driver's probe asks for capset information and waits five
	 * seconds for the answer. This transport's initcall runs about fifty
	 * milliseconds into boot, while the daemon that answers is a Windows
	 * process the launcher starts moments AFTER the guest -- so the query
	 * went out before anyone was listening, timed out, and the driver came
	 * up with no usable capset. Mesa then reported "No virgl contexts
	 * available on host" and fell back to llvmpipe, which looks exactly
	 * like a device that does not work.
	 *
	 * So registration is deferred to a thread that waits for the daemon to
	 * announce itself. Boot is not held up: if no daemon ever appears the
	 * thread gives up quietly and the guest simply has no GPU, which is
	 * the correct outcome for a host that is not offering one.
	 */
	if (READ_ONCE(co_colinux_vgpu_io.enabled) == 0) {
		struct task_struct *t;

		t = kthread_run(co_vgpu_wait_and_register, cd, "covgpu-wait");
		if (!IS_ERR(t)) {
			pr_info("colinux vgpu: waiting for the host daemon\n");
			return 0;
		}
		/* No thread: fall through and register now, so a host that is
		 * already serving still works. */
	}

	rc = register_virtio_device(&cd->vdev);
	if (rc) {
		pr_err("colinux vgpu: register_virtio_device failed (%d)\n", rc);
		co_vgpu = NULL;
		platform_device_unregister(cd->pdev);
		kfree(cd);
		return rc;
	}

	pr_info("colinux vgpu: device registered, %u capsets, %u scanouts%s\n",
		co_colinux_vgpu_io.num_capsets, co_colinux_vgpu_io.num_scanouts,
		co_colinux_vgpu_io.num_scanouts ? "" : " (render-only, KMS off)");
	return 0;
}

/*
 * device_initcall, not a later one: virtio_gpu's own initcall has to find the
 * bus populated, and DRM wants to come up before userspace does.
 */
device_initcall(co_vgpu_init);

/*
 * The ABI, asserted rather than documented.
 *
 * The host's copy of these structures lives in src/colinux/kernel/vgpu.h with
 * an equivalent set of assertions. Two toolchains, two kernels, one layout --
 * and the only thing keeping them honest is that both refuse to build when
 * they disagree.
 */
static void __maybe_unused co_vgpu_abi_asserts(void)
{
	BUILD_BUG_ON(sizeof(struct co_vgpu_vq) != 32);
	BUILD_BUG_ON(offsetof(struct co_vgpu_vq, desc_gpa)  != 0);
	BUILD_BUG_ON(offsetof(struct co_vgpu_vq, avail_gpa) != 8);
	BUILD_BUG_ON(offsetof(struct co_vgpu_vq, used_gpa)  != 16);
	BUILD_BUG_ON(offsetof(struct co_vgpu_vq, num)	    != 24);
	BUILD_BUG_ON(offsetof(struct co_vgpu_vq, kick)	    != 28);

	BUILD_BUG_ON(offsetof(struct co_vgpu_io, magic)		 != 0);
	BUILD_BUG_ON(offsetof(struct co_vgpu_io, abi_version)	 != 4);
	BUILD_BUG_ON(offsetof(struct co_vgpu_io, host_features)	 != 8);
	BUILD_BUG_ON(offsetof(struct co_vgpu_io, guest_features) != 16);
	BUILD_BUG_ON(offsetof(struct co_vgpu_io, status)	 != 24);
	BUILD_BUG_ON(offsetof(struct co_vgpu_io, enabled)	 != 28);
	BUILD_BUG_ON(offsetof(struct co_vgpu_io, num_capsets)	 != 32);
	BUILD_BUG_ON(offsetof(struct co_vgpu_io, num_scanouts)	 != 36);
	BUILD_BUG_ON(offsetof(struct co_vgpu_io, vq)		 != 40);
	BUILD_BUG_ON(offsetof(struct co_vgpu_io, used_pending)	 != 104);
	BUILD_BUG_ON(offsetof(struct co_vgpu_io, reaped)	 != 108);
	BUILD_BUG_ON(sizeof(struct co_vgpu_io)			 != 120);
}
