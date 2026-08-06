/*
 * The X connection, carried in guest RAM instead of over the network.
 *
 * Measured on the M92p, guest to host, 800 MiB each way:
 *
 *     one TCP stream through slirp     15.3 MiB/s
 *     four streams                     26.4 MiB/s
 *     eight streams                    26.6 MiB/s   (32.3 peak at the receiver)
 *
 * Two separate ceilings, and the X connection is unlucky enough to sit under
 * the lower one. A single TCP stream is paced by its own acknowledgements and
 * extracts 15.3 of an available 32; slirp's userspace TCP and NAT is the 32.
 * Neither has anything to do with how fast memory is, and guest RAM *is* host
 * RAM -- the daemon already maps all of it through R3's windows -- so a frame
 * that VirtualGL has finished is one memcpy away from the X server rather than
 * a round trip through two TCP stacks and a NAT.
 *
 * What this replaces is exactly one arrow: the wire between the guest's libX11
 * and VcXsrv. Not the renderer, not VirtualGL, not the interposition, not the
 * window. The app still creates an ordinary X window on VcXsrv, VirtualGL
 * still renders on the card and still presents with XPutImage into that same
 * window; the bytes of that XPutImage simply stop going through slirp.
 *
 *   guest                                        host
 *   -----                                        ----
 *   libX11 (DISPLAY=:0)                          cogpu-daemon
 *     -> /tmp/.X11-unix/X0                         reads the ring in place
 *     -> coxwire shim  -> TX ring ---------------> -> 127.0.0.1:6000 (VcXsrv)
 *     <- coxwire shim  <- RX ring <--------------- <- VcXsrv
 *
 * Bootstrap goes over slirp because it has to start somewhere: the shim opens
 * one short TCP connection per X connection, names the pages its rings live
 * on, and never uses it for data again. Slow is fine for a few hundred bytes
 * once.
 *
 * Flow control is the counters themselves. head - tail is the occupancy, the
 * writer stalls when the ring is full and the reader when it is empty, and
 * there is no acknowledgement anywhere in the loop -- which is the entire
 * reason the 15.3 MiB/s pacing disappears.
 */

#ifndef COXWIRE_H
#define COXWIRE_H

#include <stdint.h>

/* "XWIR". Sent first on the control connection so a stray client that finds
 * the port is refused rather than interpreted. */
#define COXWIRE_MAGIC		0x58574952u

/* The control port, on the host, reached from the guest as 10.0.2.2. */
#define COXWIRE_PORT		7654

/*
 * Ring size, per direction.
 *
 * A 1080p frame is 8.29 MB, which is larger than this on purpose: the host
 * drains continuously, so the ring only has to absorb the burst between two
 * drains, not a whole frame. Four megabytes is 1024 pages, and the page list
 * for both rings plus the header then fits comfortably inside one control
 * message.
 */
#define COXWIRE_RING_BYTES	(4u * 1024u * 1024u)

/* Both rings and the header, in pages. Bounds the control message and the
 * host's per-channel page table. */
#define COXWIRE_MAX_PAGES	2100

#define COXWIRE_PAGE		4096

/*
 * The header, in the first page of the shared region.
 *
 * One writer per counter, so no locks: the guest owns tx_head and rx_tail, the
 * host owns tx_tail and rx_head. Counters are monotonic and never wrap in any
 * practical run (2^64 bytes), so occupancy is a plain subtraction and there is
 * no empty/full ambiguity to resolve. The index into the data area is the
 * counter modulo the ring size.
 *
 * Volatile is necessary but not sufficient -- both sides also need a barrier
 * between writing the payload and publishing the counter, or the reader can
 * see a counter that promises bytes the writer has not stored yet. Each side
 * does that in its own idiom; see the store_release helpers.
 */
struct coxwire_hdr {
	volatile uint64_t tx_head;	/* guest publishes: bytes written  */
	volatile uint64_t tx_tail;	/* host publishes:  bytes consumed */
	volatile uint64_t rx_head;	/* host publishes:  bytes written  */
	volatile uint64_t rx_tail;	/* guest publishes: bytes consumed */
	volatile uint32_t closed;	/* either side, once, on teardown  */
	volatile uint32_t kick;		/* guest bumps when it leaves idle */
	uint32_t ring_bytes;
	uint32_t pad;
};

/*
 * The control message: guest to host, once, when an X client connects.
 *
 * Followed immediately by npages 64-bit guest-physical page addresses, in
 * order, covering the header page and then both ring data areas. The host
 * turns each into a pointer of its own with resolve_gpa() and never needs the
 * guest's virtual addresses at all.
 */
struct coxwire_open {
	uint32_t magic;
	uint32_t chan;			/* the guest's id for this connection */
	uint32_t ring_bytes;
	uint32_t npages;
	/* uint64_t gpa[npages] follows */
};

/* Where the two ring data areas start, in bytes from the region's base. */
#define COXWIRE_TX_OFF		((uint32_t)COXWIRE_PAGE)
#define COXWIRE_RX_OFF		(COXWIRE_TX_OFF + COXWIRE_RING_BYTES)
#define COXWIRE_REGION_BYTES	(COXWIRE_RX_OFF + COXWIRE_RING_BYTES)

#endif
