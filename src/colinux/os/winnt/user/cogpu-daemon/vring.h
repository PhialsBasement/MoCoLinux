/*
 * The split virtqueue, read and written from the host side.
 *
 * This is the whole of the daemon's contact with the guest: a descriptor
 * table, an available ring the guest writes, and a used ring the host writes.
 * All three live in guest memory, which R3 has mapped into this process, so
 * every access below is an ordinary pointer dereference rather than a copy or
 * an ioctl.
 *
 * The layout is virtio 1.x split-ring, and it is not ours to choose -- the
 * guest's stock vring code defines it. Reproduced here rather than included
 * from a kernel header because this is a Windows program built by a different
 * toolchain, the same reason the transport ABI is declared twice.
 *
 * Nothing here allocates and nothing here blocks.
 */

#ifndef __COGPU_VRING_H__
#define __COGPU_VRING_H__

#include <stdint.h>

#define VRING_DESC_F_NEXT	1
#define VRING_DESC_F_WRITE	2
#define VRING_DESC_F_INDIRECT	4

struct vring_desc {
	uint64_t addr;		/* guest pseudo-physical */
	uint32_t len;
	uint16_t flags;
	uint16_t next;
};

struct vring_avail {
	uint16_t flags;
	uint16_t idx;
	uint16_t ring[];	/* size num */
};

struct vring_used_elem {
	uint32_t id;		/* head descriptor index */
	uint32_t len;		/* bytes written into it */
};

struct vring_used {
	uint16_t flags;
	uint16_t idx;
	struct vring_used_elem ring[];	/* size num */
};

/*
 * One queue as this daemon sees it: three host pointers and the counters that
 * say how far it has got.
 */
struct cogpu_vring {
	struct vring_desc  *desc;
	struct vring_avail *avail;
	struct vring_used  *used;
	uint32_t	    num;
	uint16_t	    last_avail;	/* how far we have consumed */
	uint16_t	    used_idx;	/* our private copy of used->idx */
};

/*
 * A chain of descriptors, flattened.
 *
 * Every buffer virtio-gpu submits is a chain: a request header the guest
 * wrote, then optionally more readable pieces, then writable pieces for the
 * response. The daemon needs them as two lists -- what it may read, and what
 * it may write -- because that split is what says which parts of the chain are
 * the request and which are the reply.
 */
#define COGPU_MAX_CHAIN 256

struct cogpu_buf {
	void	*addr;
	uint32_t len;
};

struct cogpu_chain {
	uint16_t	 head;		/* the id to publish in the used ring */
	int		 in_count;	/* readable by the host: the request */
	int		 out_count;	/* writable by the host: the reply */
	struct cogpu_buf in[COGPU_MAX_CHAIN];
	struct cogpu_buf out[COGPU_MAX_CHAIN];
	uint32_t	 in_bytes;
	uint32_t	 out_bytes;
};

/*
 * Resolve a guest physical address to something this process can dereference.
 * Supplied by the caller because only it knows the R3 mapping table.
 */
typedef void *(*cogpu_resolve_fn)(void *ctx, uint64_t gpa, uint32_t len);

/* Returns 1 and fills `chain` if the guest has queued something, else 0. */
int  cogpu_vring_pop(struct cogpu_vring *vr, cogpu_resolve_fn resolve,
		     void *ctx, struct cogpu_chain *chain);

/* Publish a completed chain, having written `written` bytes into its out list. */
void cogpu_vring_push(struct cogpu_vring *vr, const struct cogpu_chain *chain,
		      uint32_t written);

/* The offline self-test; returns 0 on success. See the comment in vring.c. */
int  cogpu_vring_selftest(void);

#endif
