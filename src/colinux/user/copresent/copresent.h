/*
 * CoPresent R2 wire ABI.
 *
 * The guest broker and Windows cogpu daemon include this exact header. The
 * control socket is used once to name two pinned guest pages; all steady-state
 * traffic is fixed-size records in those pages. A record never contains pixel
 * data, a host GL name, or a guest pointer.
 */
#ifndef MOCO_COPRESENT_H
#define MOCO_COPRESENT_H

#include <stdint.h>

#define COPRESENT_MAGIC          0x5250434du /* "MCPR" little-endian */
#define COPRESENT_VERSION        1u
#define COPRESENT_CONTROL_PORT   7655
#define COPRESENT_PAGE_BYTES     4096u
#define COPRESENT_REGION_BYTES   (2u * COPRESENT_PAGE_BYTES)
#define COPRESENT_REGION_PAGES   2u
#define COPRESENT_RING_SLOTS     32u
#define COPRESENT_LOCAL_PATH     "/run/moco-present.sock"

enum copresent_opcode {
	COPRESENT_BIND    = 1,
	COPRESENT_PRESENT = 2,
	COPRESENT_UNBIND  = 3,

	COPRESENT_BOUND   = 0x101,
	COPRESENT_RELEASE = 0x102,
	COPRESENT_UNBOUND = 0x103,
	COPRESENT_ERROR   = 0x1ff
};

enum copresent_status {
	COPRESENT_OK             = 0,
	COPRESENT_BAD_RECORD     = 1,
	COPRESENT_NO_RESOURCE    = 2,
	COPRESENT_NO_WINDOW      = 3,
	COPRESENT_STALE          = 4,
	COPRESENT_PRESENT_FAILED = 5
};

#define COPRESENT_F_ACQUIRE_WAITED (1u << 0)
#define COPRESENT_F_Y_0_TOP        (1u << 1)

/* Sent over TCP once, followed by npages uint64_t guest-physical addresses. */
struct copresent_open {
	uint32_t magic;
	uint32_t version;
	uint32_t generation;
	uint32_t npages;
};

/* Exactly one cache line. Damage is advisory in R2 but already part of the
 * versioned ABI so later rungs do not have to reinterpret a PRESENT record. */
struct copresent_record {
	uint32_t opcode;
	uint32_t version;
	uint32_t generation;
	uint32_t connection;
	uint32_t buffer;
	uint32_t serial;
	uint32_t xid;
	uint32_t resource_id;
	uint32_t width;
	uint32_t height;
	uint32_t damage_x;
	uint32_t damage_y;
	uint32_t damage_width;
	uint32_t damage_height;
	uint32_t flags;
	uint32_t status;
};

/* One writer owns each counter. Guest writes tx_head/rx_tail; host writes
 * tx_tail/rx_head. Monotonic subtraction is valid while occupancy is bounded
 * by COPRESENT_RING_SLOTS, including across uint32_t wrap. */
struct copresent_region {
	volatile uint32_t magic;
	volatile uint32_t version;
	volatile uint32_t generation;
	volatile uint32_t closed;
	volatile uint32_t tx_head;
	volatile uint32_t tx_tail;
	volatile uint32_t rx_head;
	volatile uint32_t rx_tail;
	volatile uint32_t guest_heartbeat;
	volatile uint32_t host_heartbeat;
	volatile uint32_t kick;
	uint32_t reserved[5];
	struct copresent_record tx[COPRESENT_RING_SLOTS];
	struct copresent_record rx[COPRESENT_RING_SLOTS];
};

typedef char copresent_record_size_check[
	(sizeof(struct copresent_record) == 64) ? 1 : -1];
typedef char copresent_region_layout_check[
	(sizeof(struct copresent_region) == 4160 &&
	 __builtin_offsetof(struct copresent_region, tx) == 64 &&
	 __builtin_offsetof(struct copresent_region, rx) == 2112 &&
	 sizeof(struct copresent_region) <= COPRESENT_REGION_BYTES) ? 1 : -1];

#endif
