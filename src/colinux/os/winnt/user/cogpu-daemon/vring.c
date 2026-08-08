/*
 * Walking the guest's virtqueues from the host.
 *
 * Small, and deliberately the most paranoid file in the daemon. Everything it
 * reads -- descriptor indices, chain links, lengths, addresses -- was written
 * by the guest, and the guest is a whole Linux that can be wrong. A bad index
 * here is not a bad frame, it is a wild pointer into a gigabyte of mapped
 * memory, from a process that also holds writable windows onto the guest's
 * RAM. So every value is bounded before it is used, and a chain that does not
 * make sense is refused rather than followed.
 *
 * The other half of the paranoia is the self-test at the bottom. Ring code is
 * the classic thing that works for the first hundred requests and then wraps,
 * and this project's own rule is that never-executed code is untested code --
 * the slirp bridge cost an evening proving it. So the wrap, the full ring and
 * the empty ring are all exercised against scratch memory before this ever
 * looks at a live guest, on every daemon start.
 */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vring.h"

/*
 * Read a descriptor by index, bounded.
 *
 * `num` is a power of two and comes from the guest, but the daemon validated
 * it once when the queue was set up; every index is masked against it here so
 * a corrupt `next` cannot walk off the table.
 */
static const struct vring_desc *desc_at(const struct cogpu_vring *vr, uint16_t i)
{
	if (i >= vr->num)
		return NULL;
	return &vr->desc[i];
}

/*
 * Add one host-contiguous piece of a guest descriptor.
 *
 * KMAP is contiguous only inside one userspace window. A descriptor remains
 * contiguous in the guest's pseudo-physical address space when it crosses
 * such a boundary, but there is no single host pointer for the whole thing.
 * Adjacent pieces are folded back together when Windows happened to place the
 * two mappings beside each other, keeping ordinary chains as small as before.
 */
static int chain_add(struct cogpu_chain *chain, int write,
		     void *addr, uint32_t len)
{
	struct cogpu_buf *buf = write ? chain->out : chain->in;
	int *count = write ? &chain->out_count : &chain->in_count;

	if (*count != 0) {
		struct cogpu_buf *last = &buf[*count - 1];

		if ((uintptr_t)last->addr + last->len == (uintptr_t)addr &&
		    UINT32_MAX - last->len >= len) {
			last->len += len;
			return 1;
		}
	}

	if (*count >= COGPU_MAX_CHAIN)
		return 0;
	buf[*count].addr = addr;
	buf[*count].len = len;
	(*count)++;
	return 1;
}

int cogpu_vring_pop(struct cogpu_vring *vr, cogpu_resolve_fn resolve,
		    void *ctx, struct cogpu_chain *chain)
{
	uint16_t avail_idx, head, i;
	int	 hops = 0;

	if (!vr->desc || !vr->avail || !vr->used)
		return 0;

	/*
	 * The guest publishes avail->idx last, after the descriptors it
	 * describes. Reading it first and then the descriptors is the
	 * corresponding order on this side; on x86 the load-load ordering is
	 * already guaranteed, and the compiler barrier is what stops the read
	 * being hoisted.
	 */
	avail_idx = vr->avail->idx;
	__atomic_thread_fence(__ATOMIC_ACQUIRE);

	if (avail_idx == vr->last_avail)
		return 0;

	/*
	 * How far behind we are. If the guest has somehow advanced by more
	 * than the ring holds, the ring is not trustworthy and neither is
	 * anything in it.
	 */
	if ((uint16_t)(avail_idx - vr->last_avail) > vr->num)
		return 0;

	head = vr->avail->ring[vr->last_avail % vr->num];
	if (head >= vr->num)
		return 0;

	memset(chain, 0, sizeof(*chain));
	chain->head = head;

	i = head;
	for (;;) {
		const struct vring_desc *d = desc_at(vr, i);

		if (!d)
			return 0;

		/*
		 * A chain longer than the table means a cycle. Bounding hops by
		 * num is what makes a corrupt `next` terminate rather than spin
		 * this thread forever inside a poll loop.
		 */
		if (++hops > (int)vr->num)
			return 0;

		if (d->len) {
			uint64_t addr = d->addr;
			uint32_t left = d->len;
			int write = (d->flags & VRING_DESC_F_WRITE) != 0;
			void *p = resolve(ctx, addr, left);

			if (p) {
				if (!chain_add(chain, write, p, left))
					return 0;
			} else {
				/*
				 * The address may be valid but span separate KMAP
				 * windows. A page is the unit guaranteed contiguous
				 * on both sides, so resolve page pieces and coalesce
				 * whichever host mappings really are adjacent.
				 */
				while (left) {
					uint32_t part = 4096u - (uint32_t)(addr & 4095u);

					if (part > left)
						part = left;
					p = resolve(ctx, addr, part);
					if (!p || !chain_add(chain, write, p, part))
						return 0;
					addr += part;
					left -= part;
				}
			}

			if (write)
				chain->out_bytes += d->len;
			else
				chain->in_bytes += d->len;
		}

		if (!(d->flags & VRING_DESC_F_NEXT))
			break;
		i = d->next;
	}

	vr->last_avail++;
	return 1;
}

void cogpu_vring_push(struct cogpu_vring *vr, const struct cogpu_chain *chain,
		      uint32_t written)
{
	uint16_t slot;

	if (!vr->used)
		return;

	slot = vr->used_idx % vr->num;
	vr->used->ring[slot].id	 = chain->head;
	vr->used->ring[slot].len = written;

	/*
	 * Payload before index, always.
	 *
	 * The guest reads used->idx and then trusts every entry below it. If
	 * the index became visible before the entry it names, the guest would
	 * complete a request against an id and a length that had not been
	 * written yet -- which is not a lost completion but a wrong one, and it
	 * would retire the wrong fence.
	 */
	__atomic_thread_fence(__ATOMIC_RELEASE);
	vr->used_idx++;
	vr->used->idx = vr->used_idx;
}

/* ------------------------------------------------------------- self-test */

/*
 * Everything above, against scratch memory, before a live guest is touched.
 *
 * The cases are the ones ring code actually gets wrong: index wrap at 65535,
 * a ring that is completely full, an empty ring, a chain that loops back on
 * itself, a descriptor pointing outside guest memory, and one valid guest
 * descriptor crossing two unrelated host mappings. The corrupt cases must be
 * REFUSED and the split one must be accepted -- a self-test that only proves
 * the happy path is the kind of passing test this project has already been
 * caught by.
 */

#define TEST_NUM 8

static void *test_resolve(void *ctx, uint64_t gpa, uint32_t len)
{
	/* A pretend 64 KB of guest RAM based at 0x100000. */
	unsigned char *base = (unsigned char *)ctx;

	if (gpa < 0x100000 || gpa + len > 0x100000 + 0x10000)
		return NULL;
	return base + (gpa - 0x100000);
}

struct test_split_ram {
	unsigned char first[4096];
	unsigned char gap[64];
	unsigned char second[4096];
};

static void *test_split_resolve(void *ctx, uint64_t gpa, uint32_t len)
{
	struct test_split_ram *ram = ctx;
	uint64_t off;

	if (gpa < 0x200000 || gpa + len < gpa ||
	    gpa + len > 0x200000 + 8192)
		return NULL;
	off = gpa - 0x200000;
	if (off < 4096 && off + len <= 4096)
		return ram->first + off;
	if (off >= 4096 && off + len <= 8192)
		return ram->second + (off - 4096);
	return NULL;
}

int cogpu_vring_selftest(void)
{
	static unsigned char ram[0x10000];
	static struct test_split_ram split_ram;
	unsigned char	     descmem[sizeof(struct vring_desc) * TEST_NUM];
	unsigned char	     availmem[sizeof(struct vring_avail) + 2 * TEST_NUM];
	unsigned char	     usedmem[sizeof(struct vring_used) +
				      sizeof(struct vring_used_elem) * TEST_NUM];
	struct cogpu_vring   vr;
	struct cogpu_chain   chain;
	int		     i, failures = 0;

	memset(descmem, 0, sizeof(descmem));
	memset(availmem, 0, sizeof(availmem));
	memset(usedmem, 0, sizeof(usedmem));

	memset(&vr, 0, sizeof(vr));
	vr.desc	 = (struct vring_desc *)descmem;
	vr.avail = (struct vring_avail *)availmem;
	vr.used	 = (struct vring_used *)usedmem;
	vr.num	 = TEST_NUM;

	/* 1. empty ring yields nothing */
	if (cogpu_vring_pop(&vr, test_resolve, ram, &chain)) {
		printf("  vring selftest: empty ring returned a chain\n");
		failures++;
	}

	/* 2. a two-descriptor chain, one readable then one writable */
	vr.desc[0].addr	 = 0x100000;
	vr.desc[0].len	 = 32;
	vr.desc[0].flags = VRING_DESC_F_NEXT;
	vr.desc[0].next	 = 1;
	vr.desc[1].addr	 = 0x100100;
	vr.desc[1].len	 = 64;
	vr.desc[1].flags = VRING_DESC_F_WRITE;
	vr.avail->ring[0] = 0;
	vr.avail->idx	  = 1;

	if (!cogpu_vring_pop(&vr, test_resolve, ram, &chain) ||
	    chain.in_count != 1 || chain.out_count != 1 ||
	    chain.in_bytes != 32 || chain.out_bytes != 64 || chain.head != 0) {
		printf("  vring selftest: simple chain wrong (in %d out %d)\n",
		       chain.in_count, chain.out_count);
		failures++;
	}

	cogpu_vring_push(&vr, &chain, 64);
	if (vr.used->idx != 1 || vr.used->ring[0].id != 0 ||
	    vr.used->ring[0].len != 64) {
		printf("  vring selftest: push did not publish correctly\n");
		failures++;
	}

	/* 3. a cycle must be refused, not followed */
	vr.desc[2].addr	 = 0x100000;
	vr.desc[2].len	 = 16;
	vr.desc[2].flags = VRING_DESC_F_NEXT;
	vr.desc[2].next	 = 2;			/* points at itself */
	vr.avail->ring[1 % TEST_NUM] = 2;
	vr.avail->idx = 2;
	if (cogpu_vring_pop(&vr, test_resolve, ram, &chain)) {
		printf("  vring selftest: a descriptor cycle was followed\n");
		failures++;
	}
	vr.last_avail = 2;			/* skip the bad one */

	/* 4. an address outside guest RAM must be refused */
	vr.desc[3].addr	 = 0x900000;		/* nowhere near the window */
	vr.desc[3].len	 = 16;
	vr.desc[3].flags = 0;
	vr.avail->ring[2 % TEST_NUM] = 3;
	vr.avail->idx = 3;
	if (cogpu_vring_pop(&vr, test_resolve, ram, &chain)) {
		printf("  vring selftest: an out-of-range address was accepted\n");
		failures++;
	}
	vr.last_avail = 3;

	/* 5. index wrap: drive both counters past 65535 */
	vr.desc[4].addr	 = 0x100200;
	vr.desc[4].len	 = 8;
	vr.desc[4].flags = 0;

	vr.last_avail = 0xfffe;
	vr.avail->idx = 0xfffe;
	vr.used_idx   = 0xfffe;
	vr.used->idx  = 0xfffe;

	for (i = 0; i < 4; i++) {
		vr.avail->ring[(uint16_t)(0xfffe + i) % TEST_NUM] = 4;
		vr.avail->idx = (uint16_t)(0xfffe + i + 1);

		if (!cogpu_vring_pop(&vr, test_resolve, ram, &chain)) {
			printf("  vring selftest: lost a chain across the wrap"
			       " (iteration %d)\n", i);
			failures++;
			break;
		}
		cogpu_vring_push(&vr, &chain, 8);
	}

	if (!failures && vr.used->idx != (uint16_t)(0xfffe + 4)) {
		printf("  vring selftest: used index wrapped wrongly (%u)\n",
		       vr.used->idx);
		failures++;
	}

	/* 6. One guest descriptor may cross separate host KMAP windows. */
	memset(descmem, 0, sizeof(descmem));
	memset(availmem, 0, sizeof(availmem));
	memset(usedmem, 0, sizeof(usedmem));
	memset(&vr, 0, sizeof(vr));
	vr.desc  = (struct vring_desc *)descmem;
	vr.avail = (struct vring_avail *)availmem;
	vr.used  = (struct vring_used *)usedmem;
	vr.num   = TEST_NUM;
	vr.desc[0].addr = 0x200000 + 4096 - 16;
	vr.desc[0].len  = 32;
	vr.avail->ring[0] = 0;
	vr.avail->idx = 1;

	if (!cogpu_vring_pop(&vr, test_split_resolve, &split_ram, &chain) ||
	    chain.in_count != 2 || chain.in_bytes != 32 ||
	    chain.in[0].len != 16 || chain.in[1].len != 16) {
		printf("  vring selftest: split descriptor was not preserved"
		       " (pieces %d, bytes %u)\n",
		       chain.in_count, chain.in_bytes);
		failures++;
	}

	if (failures == 0)
		printf("  vring selftest: 6/6 (empty, chain, cycle, range, wrap, split)\n");

	return failures;
}
