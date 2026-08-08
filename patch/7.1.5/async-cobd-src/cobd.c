// SPDX-License-Identifier: GPL-2.0
/*
 * cobd -- the cooperative block device.
 *
 * The guest has no disk controller and must never touch the host's. Its
 * storage is a file or a partition the host holds open, and a transfer is a
 * world switch: the guest fills in a request in the passage page, calls the
 * switch, and the host performs the I/O straight into the guest's memory
 * before switching back.
 *
 * That the host can write into the guest's buffer directly is the whole reason
 * this driver is short. page_to_phys() yields a dense guest pseudo-physical
 * address; the host resolves it through p2m to the scattered machine frame and
 * addresses that frame directly. There is no bounce buffer, no scatter list
 * handed to hardware, and no DMA mapping.
 *
 * The transfer is synchronous: it is complete by the time the switch returns,
 * so there is no completion interrupt, no request tracking and no timeout
 * path. The cost is that the guest blocks for the duration of host I/O, which
 * for a cooperative guest is the same trade every other yield makes.
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/blkdev.h>
#include <linux/blk-mq.h>
#include <linux/console.h>	/* co_colinux_guest */
#include <linux/major.h>

#define COBD_MAX_UNITS		4
#define COBD_MINORS		16	/* unit + 15 partitions */
#define COBD_SECTOR_SIZE	512

/* Passage-page offsets and operations -- see arch/x86/kernel/process.c. */
#define CO_PP_CODE		0x38
#define CO_PP_HOST_STATE	0x740
#define CO_PP_LINUXVM_STATE	0x880
#define CO_PP_OPERATION		0x9c0
#define CO_PP_PARAMS		0x9c8	/* params[0] */

#define CO_PP_PARAM(n)		(CO_PP_PARAMS + (n) * 8)

#define CO_OPERATION_BLOCK_PROBE	17
#define CO_OPERATION_BLOCK_IO		18

extern unsigned long long co_colinux_passage_page;

struct cobd_dev {
	struct gendisk		*disk;
	struct blk_mq_tag_set	tag_set;
	unsigned long long	size;
	int			unit;
};

/*
 * One descriptor per physically contiguous run of a request, handed to the host
 * as an array so a whole request costs one world crossing.
 *
 * Sixteen bytes and naturally aligned, so a descriptor never straddles a page
 * and the host resolves each with one frame lookup.
 */
struct cobd_sg {
	__u64	pa;
	__u32	len;
	__u32	pad;
};

/*
 * Per-request through blk-mq's cmd_size rather than a shared buffer: the queue
 * is sixteen deep, so a static array could be handed to the host while another
 * request was still filling it.
 */
struct cobd_cmd {
	struct cobd_sg	sg[BLK_MAX_SEGMENTS];
};

static struct cobd_dev cobd_devs[COBD_MAX_UNITS];
static int cobd_major;

/*
 * Asynchronous block I/O -- defect 3 (doc/async-cobd-design).
 *
 * The synchronous path (cobd_io_sg) blocks the crossing until the host has
 * finished the whole transfer; on a single-CPU cooperative guest that freezes
 * everything -- the console, the timer, systemd's watchdogs -- for the duration
 * of the disk I/O. Async instead submits the request and returns at once: the
 * host queues it to a worker thread and posts the result into this completion
 * ring, which the guest reaps on the cooperative tick.
 *
 * co_colinux_cobd_io is resolved by the host from the image by name, exactly as
 * co_colinux_net_io is, and its layout is declared ABI -- kernel/cobd_async.c
 * carries matching CO_CIO_* offsets. The host is the sole producer into comp[]
 * and advances comp_head; the guest is the sole consumer and advances comp_tail.
 * enabled is written once by the host at boot to switch this path on.
 */
#define CO_COBD_COMP_SLOTS	128		/* power of two, > 64 max outstanding */

struct co_cobd_comp {
	__u32 unit;
	__u32 tag;
	__u32 result;			/* 0 = OK, 1 = I/O error */
	__u32 pad;
};

struct co_cobd_io {
	volatile __u32 enabled;		/* host sets 1 at boot if async is on */
	volatile __u32 comp_head;	/* host writes (producer) */
	volatile __u32 comp_tail;	/* guest writes (consumer) */
	volatile __u32 pad;
	struct co_cobd_comp comp[CO_COBD_COMP_SLOTS];
};

struct co_cobd_io co_colinux_cobd_io;
EXPORT_SYMBOL(co_colinux_cobd_io);

void co_cobd_drain_completions(void);

/*
 * Reap finished requests. Called from the cooperative tick and the idle /
 * exit-to-user drains (arch/x86/kernel/process.c), so completion latency is one
 * tick (HZ=1000, <=1 ms) at worst and immediate whenever the guest returns to
 * userspace. Harmless when async is off: the ring is zeroed .bss, head == tail,
 * and the loop does not run.
 *
 * The tag is the blk-mq request tag, resolved with blk_mq_tag_to_rq -- a
 * corrupted record can then only name a NULL or an unstarted request, both
 * ignored, never an arbitrary kernel pointer. blk_mq_request_started guards a
 * tag already ended and recycled, so a stray duplicate cannot double-complete.
 */
void co_cobd_drain_completions(void)
{
	struct co_cobd_io *io = &co_colinux_cobd_io;
	__u32 head = io->comp_head;
	__u32 tail = io->comp_tail;

	if (head == tail)
		return;

	/*
	 * A gap wider than the ring, or a nonsense unit, means the producer and
	 * consumer have lost sync -- a bug, not weather. Resync rather than walk
	 * garbage, matching conet's stance.
	 */
	if ((__u32)(head - tail) > CO_COBD_COMP_SLOTS) {
		io->comp_tail = head;
		return;
	}

	while (tail != head) {
		struct co_cobd_comp *c;
		struct cobd_dev *dev;
		struct request *rq;

		smp_rmb();		/* pairs with the host's store order */
		c = &io->comp[tail & (CO_COBD_COMP_SLOTS - 1)];

		if (c->unit < COBD_MAX_UNITS) {
			dev = &cobd_devs[c->unit];
			if (dev->disk && c->tag < dev->tag_set.queue_depth) {
				rq = blk_mq_tag_to_rq(dev->tag_set.tags[0], c->tag);
				if (rq && blk_mq_request_started(rq))
					blk_mq_end_request(rq,
						c->result ? BLK_STS_IOERR
							  : BLK_STS_OK);
			}
		}
		tail++;
	}

	io->comp_tail = tail;		/* x86 store order: after the reads above */
}
EXPORT_SYMBOL(co_cobd_drain_completions);

/*
 * The switch call itself.
 *
 * Identical in shape to co_colinux_switch() in process.c, and deliberately a
 * separate copy rather than an export: this one must not be instrumented or
 * traced, and it is called from block submission context where a detour
 * through the tracer would be a poor idea.
 */
static void cobd_switch(unsigned long long operation)
{
	unsigned char *pp = (unsigned char *)co_colinux_passage_page;
	register void *leaving  asm("rcx") = pp + CO_PP_LINUXVM_STATE;
	register void *entering asm("rdx") = pp + CO_PP_HOST_STATE;
	void *code = pp + CO_PP_CODE;

	*(volatile unsigned long long *)(pp + CO_PP_OPERATION) = operation;

	asm volatile(
		"call *%[code]\n"
		: "+r"(leaving), "+r"(entering)
		: [code] "r"(code)
		: "rax", "rsi", "rdi", "r8", "r9", "r10", "r11", "memory", "cc");
}

/*
 * Hand the host a whole request: a start offset and a list of scattered
 * physical runs, transferred in one crossing.
 *
 * It used to be one call per segment, and the block layer produces one segment
 * per page because page-cache pages are not physically contiguous. A 128 KB
 * readahead was therefore thirty-two world crossings, each with its own
 * synchronous host file read, and the crossing is the expensive part.
 *
 * Asking for larger segments was tried first and did nothing, for that same
 * reason: rq_for_each_bvec only yields a multi-page run when the pages happen
 * to be adjacent in physical memory, which for readahead they are not. The fix
 * is not to ask for contiguity but to stop needing it -- describe the scattered
 * pages once, cross once.
 *
 * Why it matters for a desktop and did not for the install: bulk streaming
 * amortised the crossing over a lot of bytes, while an application paging in Qt
 * and the KDE frameworks is thousands of small scattered reads that each paid
 * in full.
 */
static int cobd_io_sg(int unit, u64 offset, phys_addr_t sg_pa, u32 count,
		      bool write)
{
	unsigned char *pp = (unsigned char *)co_colinux_passage_page;
	int ret;

	/*
	 * The parameters and the crossing are one operation and must not be
	 * split by the scheduler.
	 *
	 * There is exactly one passage page. These six words are not this
	 * request's -- they are the machine's only set, and they stay meaningful
	 * for precisely as long as it takes to get from the last store to the
	 * call below. queue_rq runs in fully preemptible process context:
	 * BLK_MQ_F_BLOCKING says it may sleep, and blk_mq_submit_bio issues
	 * directly in the submitting task's own context, so two tasks reach here
	 * independently and CONFIG_PREEMPTION lets one displace the other
	 * mid-sequence.
	 *
	 * Interleaved, that is: A writes its six words, is preempted, B writes
	 * its own over the top and crosses, the host performs B's transfer, B
	 * reads success and returns. A is then rescheduled and crosses with B's
	 * parameters still in the page. The host obligingly performs B's
	 * transfer a second time; A reads params[56], finds the zero B's
	 * transfer left there, and reports success for a read that never
	 * happened. The block layer marks those pages uptodate holding whatever
	 * was in them, and they are program text and page tables on their way
	 * in.
	 *
	 * The result was a guest that died within a second of starting anything
	 * that pages in a lot from several threads at once -- every Qt
	 * application -- while xterm, which is one thread touching a handful of
	 * small libraries, ran fine. Sequential dd measured 140 MB/s and was
	 * never once wrong, because one thread issuing one request at a time
	 * cannot race itself. Nothing is logged when this fires: both crossings
	 * succeed and the corruption is delivered as data.
	 *
	 * preempt_disable is the whole fix and the right size of it. It is not
	 * an SMP question -- the guest has one processor -- and interrupts do
	 * not need masking either, because vectors 32-255 belong to the host and
	 * never run guest code. What has to be prevented is the guest's own
	 * scheduler putting another task on the processor in the middle, and
	 * that is exactly what the preemption count prevents.
	 */
	preempt_disable();

	*(volatile u64 *)(pp + CO_PP_PARAM(51)) = unit;
	*(volatile u64 *)(pp + CO_PP_PARAM(52)) = offset;
	*(volatile u64 *)(pp + CO_PP_PARAM(53)) = count;
	*(volatile u64 *)(pp + CO_PP_PARAM(54)) = sg_pa;
	*(volatile u64 *)(pp + CO_PP_PARAM(55)) = write ? 1 : 0;
	*(volatile u64 *)(pp + CO_PP_PARAM(56)) = ~0ULL;

	cobd_switch(CO_OPERATION_BLOCK_IO);

	ret = *(volatile u64 *)(pp + CO_PP_PARAM(56)) ? -EIO : 0;

	preempt_enable();

	return ret;
}

/*
 * Async submit: hand the host the same request as cobd_io_sg, plus the blk-mq
 * tag in params[57], and return whether it was accepted rather than the I/O
 * result. The host queues it and returns immediately; params[56] now means
 * accepted(0)/busy(1), and the actual result comes back later through the
 * completion ring. Returns true if accepted.
 *
 * Same passage-page rule as cobd_io_sg: the param writes and the crossing are
 * one operation under preempt_disable, because there is a single set of params
 * and another task must not overwrite them mid-sequence.
 */
static bool cobd_submit_async(int unit, u64 offset, phys_addr_t sg_pa, u32 count,
			      bool write, unsigned int tag)
{
	unsigned char *pp = (unsigned char *)co_colinux_passage_page;
	bool accepted;

	preempt_disable();

	*(volatile u64 *)(pp + CO_PP_PARAM(51)) = unit;
	*(volatile u64 *)(pp + CO_PP_PARAM(52)) = offset;
	*(volatile u64 *)(pp + CO_PP_PARAM(53)) = count;
	*(volatile u64 *)(pp + CO_PP_PARAM(54)) = sg_pa;
	*(volatile u64 *)(pp + CO_PP_PARAM(55)) = write ? 1 : 0;
	*(volatile u64 *)(pp + CO_PP_PARAM(57)) = tag;
	*(volatile u64 *)(pp + CO_PP_PARAM(56)) = ~0ULL;

	cobd_switch(CO_OPERATION_BLOCK_IO);

	accepted = (*(volatile u64 *)(pp + CO_PP_PARAM(56)) == 0);

	preempt_enable();

	return accepted;
}

static u64 cobd_probe(int unit)
{
	unsigned char *pp = (unsigned char *)co_colinux_passage_page;
	u64 size;

	/* Same page, same rule -- see cobd_io_sg(). This one runs at init with
	 * nothing else to race, which is a fact about today rather than a
	 * property of the code. */
	preempt_disable();

	*(volatile u64 *)(pp + CO_PP_PARAM(51)) = unit;
	*(volatile u64 *)(pp + CO_PP_PARAM(52)) = 0;

	cobd_switch(CO_OPERATION_BLOCK_PROBE);

	size = *(volatile u64 *)(pp + CO_PP_PARAM(52));

	preempt_enable();

	return size;
}

static blk_status_t cobd_queue_rq(struct blk_mq_hw_ctx *hctx,
				  const struct blk_mq_queue_data *bd)
{
	struct request *req = bd->rq;
	struct cobd_dev *dev = req->q->queuedata;
	blk_status_t status = BLK_STS_OK;
	struct req_iterator iter;
	struct cobd_cmd *cmd;
	struct bio_vec bvec;
	unsigned int n;
	u64 pos;

	blk_mq_start_request(req);

	switch (req_op(req)) {
	case REQ_OP_READ:
	case REQ_OP_WRITE:
		pos = (u64)blk_rq_pos(req) << SECTOR_SHIFT;

		/*
		 * rq_for_each_bvec, not rq_for_each_segment.
		 *
		 * The segment form splits at every page, so a 128 KB readahead
		 * became thirty-two calls to cobd_io() and therefore thirty-two
		 * world crossings, each with its own synchronous host file read.
		 * The crossing is the expensive part and it was being paid per
		 * page. The bvec form yields whole physically contiguous runs,
		 * so the same readahead is one crossing.
		 *
		 * This is safe because the host resolves the buffer itself: see
		 * co_cobd_request() in kernel/cobd.c, which walks the transfer a
		 * page at a time, looks each frame up separately, and refuses an
		 * address that is not guest memory. The per-page limits below
		 * existed for a version of that function which resolved one
		 * frame per call, and are what this comment used to say.
		 *
		 * It matters most for exactly the workload that was unusable:
		 * bulk streaming amortised the crossing over a lot of bytes and
		 * ran fine, while a desktop application paging in Qt and the KDE
		 * frameworks is thousands of small scattered reads and paid the
		 * full cost on every one.
		 */
		cmd = blk_mq_rq_to_pdu(req);
		n = 0;

		rq_for_each_bvec(bvec, req, iter) {
			if (n == BLK_MAX_SEGMENTS) {
				/*
				 * Cannot happen -- max_segments is the same
				 * bound the block layer builds to -- but the
				 * alternative to checking is writing past the
				 * array into the request's own private data.
				 */
				status = BLK_STS_IOERR;
				break;
			}

			cmd->sg[n].pa  = page_to_phys(bvec.bv_page) +
					 bvec.bv_offset;
			cmd->sg[n].len = bvec.bv_len;
			cmd->sg[n].pad = 0;
			n++;
		}

		if (status != BLK_STS_OK || n == 0)
			break;

		if (co_colinux_cobd_io.enabled) {
			/*
			 * Async (default): submit and leave the request
			 * outstanding. The PDU (cmd->sg) stays valid until the
			 * completion ends the request, which is exactly what
			 * lets the host read the scatter list from a worker
			 * thread later. A full host queue returns busy -> ask
			 * blk-mq to requeue (backpressure, never a dropped or
			 * failed request). Do NOT end the request here.
			 */
			if (cobd_submit_async(dev->unit, pos,
					      virt_to_phys(cmd->sg), n,
					      op_is_write(req_op(req)),
					      req->tag))
				return BLK_STS_OK;
			return BLK_STS_DEV_RESOURCE;
		}

		/* Sync fallback (--sync-cobd): transfer inline, complete below. */
		if (cobd_io_sg(dev->unit, pos, virt_to_phys(cmd->sg), n,
			       op_is_write(req_op(req))))
			status = BLK_STS_IOERR;
		break;

	/*
	 * The host writes through -- ZwWriteFile against a handle opened
	 * without caching promises, and nothing of the transfer is left
	 * anywhere the guest owns once the switch returns. So a flush has
	 * nothing left to do and succeeds rather than being refused, which
	 * would fail every journal commit ext4 makes.
	 */
	case REQ_OP_FLUSH:
		break;

	default:
		status = BLK_STS_NOTSUPP;
		break;
	}

	blk_mq_end_request(req, status);

	return BLK_STS_OK;
}

static const struct blk_mq_ops cobd_mq_ops = {
	.queue_rq = cobd_queue_rq,
};

static const struct block_device_operations cobd_fops = {
	.owner = THIS_MODULE,
};

static int cobd_add_disk(struct cobd_dev *dev, int unit, u64 size)
{
	struct queue_limits lim = {
		.logical_block_size	= COBD_SECTOR_SIZE,
		.physical_block_size	= COBD_SECTOR_SIZE,
		/*
		 * Large segments, and merging across pages.
		 *
		 * These were one page and no merging, on the grounds that a
		 * segment spanning pages could span two host allocations that
		 * are not adjacent -- guest memory is several separate
		 * contiguous blocks, so guest-physical adjacency does not imply
		 * host-virtual adjacency. That is still true, and it is handled
		 * where it belongs: co_cobd_request() walks the transfer a page
		 * at a time and resolves each frame on its own.
		 *
		 * Keeping the limit here as well cost a world crossing per page.
		 * A megabyte lets the block layer build runs worth crossing for
		 * while staying well inside what one request can describe.
		 */
		.max_segment_size	= 1024 * 1024,
		.max_segments		= BLK_MAX_SEGMENTS,
	};
	struct gendisk *disk;
	int err;

	dev->unit = unit;
	dev->size = size;

	dev->tag_set.ops		= &cobd_mq_ops;
	dev->tag_set.nr_hw_queues	= 1;
	dev->tag_set.queue_depth	= 16;
	dev->tag_set.numa_node		= NUMA_NO_NODE;
	dev->tag_set.cmd_size		= sizeof(struct cobd_cmd);
	dev->tag_set.flags		= BLK_MQ_F_BLOCKING;
	dev->tag_set.driver_data	= dev;

	err = blk_mq_alloc_tag_set(&dev->tag_set);
	if (err)
		return err;

	disk = blk_mq_alloc_disk(&dev->tag_set, &lim, dev);
	if (IS_ERR(disk)) {
		err = PTR_ERR(disk);
		blk_mq_free_tag_set(&dev->tag_set);
		return err;
	}

	dev->disk		= disk;
	disk->major		= cobd_major;
	disk->first_minor	= unit * COBD_MINORS;
	disk->minors		= COBD_MINORS;
	disk->fops		= &cobd_fops;
	disk->private_data	= dev;
	disk->queue->queuedata	= dev;
	snprintf(disk->disk_name, DISK_NAME_LEN, "cobd%d", unit);
	set_capacity(disk, size >> SECTOR_SHIFT);

	err = add_disk(disk);
	if (err) {
		put_disk(disk);
		blk_mq_free_tag_set(&dev->tag_set);
		dev->disk = NULL;
		return err;
	}

	pr_info("cobd%d: %llu MB\n", unit, size >> 20);

	return 0;
}

static int __init cobd_init(void)
{
	int unit, found = 0;

	/*
	 * The completion ring is declared ABI shared with the host's hand-written
	 * CO_CIO_* offsets in kernel/cobd_async.c. Pin the layout so the two
	 * cannot drift silently.
	 */
	BUILD_BUG_ON(sizeof(struct co_cobd_comp) != 16);
	BUILD_BUG_ON(offsetof(struct co_cobd_io, comp) != 0x10);
	BUILD_BUG_ON(CO_COBD_COMP_SLOTS & (CO_COBD_COMP_SLOTS - 1));

	if (!co_colinux_guest || !co_colinux_passage_page)
		return 0;

	cobd_major = register_blkdev(0, "cobd");
	if (cobd_major < 0) {
		pr_err("cobd: cannot register a major number\n");
		return cobd_major;
	}

	for (unit = 0; unit < COBD_MAX_UNITS; unit++) {
		u64 size = cobd_probe(unit);

		/*
		 * Size zero means the host has nothing attached to this unit.
		 * A unit whose backing store is smaller than one sector is
		 * refused too, rather than registered as a disk with no
		 * sectors for the partition scanner to read.
		 */
		if (size < COBD_SECTOR_SIZE)
			continue;

		if (cobd_add_disk(&cobd_devs[unit], unit, size) == 0)
			found++;
	}

	if (!found) {
		pr_info("cobd: no devices attached by the host\n");
		unregister_blkdev(cobd_major, "cobd");
		cobd_major = 0;
	}

	return 0;
}

/*
 * device_initcall rather than module_init's usual placement: the root
 * filesystem is mounted from an initcall that runs after this level, so the
 * disk has to exist by then, and nothing here depends on a subsystem that
 * initialises later.
 */
device_initcall(cobd_init);
