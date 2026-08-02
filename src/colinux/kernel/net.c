/*
 * The host half of the guest's network rings -- the read-only half of it.
 *
 * The guest keeps two byte rings in an ordinary kernel object
 * (co_colinux_net_io, drivers/net/conet_colinux.c). They are reached the way
 * the console's rings are, by walking the guest's page tables with
 * co_kload_read, and they are guarded the way the console's are, because the
 * console's guard was bought with two minidumps: a lock protecting the
 * *address* against the guest going away underneath a client from another
 * process. The rings themselves need no lock -- one writer per word per
 * direction -- but a walk through freed page tables is a bugcheck, and
 * clearing the address without a lock still races a client between its check
 * and its walk.
 *
 * This file deliberately implements only observation. co_net_dump copies
 * indices and TX ring bytes out and writes nothing back, not even tx_tail --
 * running it twice returns the same bytes, and the guest cannot tell it
 * happened. Consuming (advancing tx_tail) and injecting (filling the RX
 * ring) are later steps, each one write wider than this.
 *
 * Offsets into struct co_net_io, which the guest side must match (the layout
 * is declared ABI in the guest driver):
 */

#include <colinux/common/common.h>
#include <colinux/common/libc.h>
#include <colinux/kernel/manager.h>
#include <colinux/kernel/kload.h>
#include <colinux/os/kernel/mutex.h>

#include "net.h"

#define CO_NIO_TX_HEAD		0x00
#define CO_NIO_TX_TAIL		0x04
#define CO_NIO_RX_HEAD		0x08
#define CO_NIO_RX_TAIL		0x0c
#define CO_NIO_TX		0x10
#define CO_NIO_TX_SIZE		(128 * 1024)
#define CO_NIO_RX		(CO_NIO_TX + CO_NIO_TX_SIZE)
#define CO_NIO_RX_SIZE		(128 * 1024)
#define CO_NIO_MAX_FRAME	1514

static unsigned long long net_io_va;
static co_os_mutex_t	  net_lock;

co_rc_t co_net_init(void)
{
	return co_os_mutex_create(&net_lock);
}

void co_net_free(void)
{
	if (net_lock) {
		co_os_mutex_destroy(net_lock);
		net_lock = NULL;
	}
}

/*
 * Called with the guest's address space live on the way in, and before that
 * space is freed on the way out. A dump either completes entirely before the
 * retirement or finds the address already zero.
 */
void co_net_set_address(unsigned long long va)
{
	if (!net_lock) {
		net_io_va = va;
		return;
	}

	co_os_mutex_acquire(net_lock);
	net_io_va = va;
	co_os_mutex_release(net_lock);
}

static co_rc_t co_net_read_u32(co_manager_t* manager, unsigned long offset,
			       unsigned int* out)
{
	return co_kload_read(manager, net_io_va + offset,
			     (unsigned char*)out, sizeof(*out));
}

/*
 * Consume: advance tx_tail, the one word in the TX ring the ABI assigns to
 * the host. This is the host's first write into guest memory on the network
 * path, and the validation is most of the point. A tail past head makes the
 * guest's fullness arithmetic (head - tail) wrap enormous and the device
 * drops everything forever -- so the new tail may only move forward, and at
 * most to the head, both read fresh under the same lock hold that performs
 * the write. In free-running u32 arithmetic one comparison covers both
 * directions: a backward tail wraps (new - cur) huge and fails the same
 * test.
 *
 * Nothing here allocates and nothing here is remembered: co_kload_write is
 * a table walk and a memcpy into frames the host already maps by arithmetic,
 * and the address it walks from is only ever read under net_lock, exactly as
 * the dump's reads are.
 */
co_rc_t co_net_take(co_manager_t* manager, unsigned int new_tail,
		    unsigned int* tx_head, unsigned int* tx_tail,
		    unsigned int* rx_head, unsigned int* rx_tail)
{
	unsigned int head, tail;
	co_rc_t rc = CO_RC(OK);

	if (!net_lock)
		return CO_RC(NOT_FOUND);

	co_os_mutex_acquire(net_lock);

	if (!net_io_va) {
		co_os_mutex_release(net_lock);
		return CO_RC(NOT_FOUND);
	}

	if (!CO_OK(co_net_read_u32(manager, CO_NIO_TX_HEAD, &head)) ||
	    !CO_OK(co_net_read_u32(manager, CO_NIO_TX_TAIL, &tail))) {
		rc = CO_RC(ERROR);
		goto out;
	}

	if (new_tail - tail > head - tail) {
		rc = CO_RC(INVALID_PARAMETER);
		goto out;
	}

	if (new_tail != tail &&
	    !CO_OK(co_kload_write(manager, net_io_va + CO_NIO_TX_TAIL,
				  (const unsigned char*)&new_tail,
				  sizeof(new_tail)))) {
		rc = CO_RC(ERROR);
		goto out;
	}

	*tx_head = head;
	*tx_tail = new_tail;

	if (!CO_OK(co_net_read_u32(manager, CO_NIO_RX_HEAD, rx_head)) ||
	    !CO_OK(co_net_read_u32(manager, CO_NIO_RX_TAIL, rx_tail)))
		rc = CO_RC(ERROR);

out:
	co_os_mutex_release(net_lock);
	return rc;
}

/*
 * Deliver frames to the guest: append records to the RX ring and publish them.
 *
 * The mirror of the guest's own transmit, and it obeys the same two rules.
 * A record is a 32-bit length, the frame, and padding to a four-byte boundary,
 * so every record starts aligned and the length word never straddles the wrap.
 * And the bytes are written before rx_head announces them -- on x86 stores are
 * not reordered with each other, so the ordinary store order here is the
 * barrier the guest's smp_rmb pairs with.
 *
 * Many frames per call, one lock hold, one publish.
 *
 * The cost of getting here is per call, not per byte: a METHOD_BUFFERED round
 * trip, a non-paged staging buffer from the I/O manager, this mutex, and a walk
 * of the guest's page tables for every co_kload_write below. Measured at about
 * a millisecond, which is why a caller doing one frame per call could not get
 * past roughly 925 frames a second however fast the network was -- a guest
 * download measured 1.39 MB/s from a mirror 5 ms away and 1.64 MB/s from
 * another continent, which is the give-away that the wire was never the limit.
 *
 * rx_head is written once, at the end, so the guest sees the whole batch appear
 * at once rather than a frame at a time. That is not just cheaper, it is the
 * same rule as before: every byte of every record is in the ring before the
 * index that announces any of them moves.
 *
 * A full ring stops the batch where it ran out rather than failing it. The
 * frames already copied are real and are published; `taken` tells the caller
 * how many, and it keeps the rest. Refusing the lot would throw away work
 * already done, and overwriting from this side is what must never happen --
 * the guest owns rx_tail and may be reading the record there.
 */
co_rc_t co_net_put(co_manager_t* manager, const unsigned char* data,
		   unsigned int size, unsigned int frames, unsigned int* taken)
{
	unsigned int head, tail, pos = 0, n = 0;
	co_rc_t rc = CO_RC(OK);

	*taken = 0;

	if (!net_lock)
		return CO_RC(NOT_FOUND);

	co_os_mutex_acquire(net_lock);

	if (!net_io_va) {
		co_os_mutex_release(net_lock);
		return CO_RC(NOT_FOUND);
	}

	if (!CO_OK(co_net_read_u32(manager, CO_NIO_RX_HEAD, &head)) ||
	    !CO_OK(co_net_read_u32(manager, CO_NIO_RX_TAIL, &tail))) {
		rc = CO_RC(ERROR);
		goto out;
	}

	if (head - tail > CO_NIO_RX_SIZE) {
		rc = CO_RC(ERROR);		/* not a ring state */
		goto out;
	}

	while (n < frames) {
		unsigned int len, record, off, first;

		/*
		 * Every record is validated against the buffer before a byte of
		 * it is believed. This data came from userspace: a length that
		 * runs past the end, or one that cannot be a frame, is a
		 * malformed request and not something to copy into the guest's
		 * memory and hope.
		 */
		if (pos + 4 > size) {
			rc = CO_RC(INVALID_PARAMETER);
			goto out;
		}

		co_memcpy(&len, data + pos, 4);

		if (len == 0 || len > CO_NIO_MAX_FRAME) {
			rc = CO_RC(INVALID_PARAMETER);
			goto out;
		}

		record = 4 + ((len + 3) & ~3u);
		if (pos + record > size) {
			rc = CO_RC(INVALID_PARAMETER);
			goto out;
		}

		if ((head - tail) + record > CO_NIO_RX_SIZE)
			break;			/* full: keep what is left */

		off = head & (CO_NIO_RX_SIZE - 1);
		if (!CO_OK(co_kload_write(manager, net_io_va + CO_NIO_RX + off,
					  (const unsigned char*)&len, 4))) {
			rc = CO_RC(ERROR);
			goto out;
		}

		off   = (off + 4) & (CO_NIO_RX_SIZE - 1);
		first = len < CO_NIO_RX_SIZE - off ? len : CO_NIO_RX_SIZE - off;

		if (!CO_OK(co_kload_write(manager, net_io_va + CO_NIO_RX + off,
					  data + pos + 4, first))) {
			rc = CO_RC(ERROR);
			goto out;
		}
		if (first < len &&
		    !CO_OK(co_kload_write(manager, net_io_va + CO_NIO_RX,
					  data + pos + 4 + first, len - first))) {
			rc = CO_RC(ERROR);
			goto out;
		}

		head += record;
		pos  += record;
		n++;
	}

	/*
	 * Publish once. If nothing fit, rx_head is not touched at all -- there
	 * is nothing to announce and no reason to write to the guest.
	 */
	if (n && !CO_OK(co_kload_write(manager, net_io_va + CO_NIO_RX_HEAD,
				       (const unsigned char*)&head, 4)))
		rc = CO_RC(ERROR);

	if (CO_OK(rc)) {
		*taken = n;
		if (n == 0)
			rc = CO_RC(OUT_OF_MEMORY);
	}

out:
	co_os_mutex_release(net_lock);
	return rc;
}

/*
 * Does the guest have frames waiting that it has not taken yet?
 *
 * Asked by the monitor loop before it sleeps on an idle yield. A guest waiting
 * for a reply is idle, so the loop sleeps -- and if the reply is already
 * sitting in the RX ring, that sleep is dead time added to every round trip.
 * The guest's receive kthread spins while there is work and only sleeps when
 * the ring is empty, so the whole latency is the host not giving it the
 * processor.
 *
 * Cheap enough to ask on every idle: two u32 reads through the guest's tables,
 * against a sleep of at least a millisecond.
 *
 * Returns false on any doubt -- no lock, no address, an unreadable index --
 * because the fallback is the sleep that has always happened.
 */
bool_t co_net_rx_pending(co_manager_t* manager)
{
	unsigned int head, tail;
	bool_t pending = PFALSE;

	if (!net_lock)
		return PFALSE;

	co_os_mutex_acquire(net_lock);

	if (net_io_va &&
	    CO_OK(co_net_read_u32(manager, CO_NIO_RX_HEAD, &head)) &&
	    CO_OK(co_net_read_u32(manager, CO_NIO_RX_TAIL, &tail)))
		pending = (head != tail);

	co_os_mutex_release(net_lock);
	return pending;
}

co_rc_t co_net_dump(co_manager_t* manager,
		    unsigned int* tx_head, unsigned int* tx_tail,
		    unsigned int* rx_head, unsigned int* rx_tail,
		    unsigned int start, unsigned char* data,
		    unsigned int* size)
{
	unsigned int want = *size;
	unsigned int off, first;
	co_rc_t rc = CO_RC(OK);

	*size = 0;

	if (!net_lock)
		return CO_RC(NOT_FOUND);

	/*
	 * Held across every page-table walk below: the guest's tables must
	 * not be freed between the check and the walks.
	 */
	co_os_mutex_acquire(net_lock);

	if (!net_io_va) {
		co_os_mutex_release(net_lock);
		return CO_RC(NOT_FOUND);
	}

	if (!CO_OK(co_net_read_u32(manager, CO_NIO_TX_HEAD, tx_head)) ||
	    !CO_OK(co_net_read_u32(manager, CO_NIO_TX_TAIL, tx_tail)) ||
	    !CO_OK(co_net_read_u32(manager, CO_NIO_RX_HEAD, rx_head)) ||
	    !CO_OK(co_net_read_u32(manager, CO_NIO_RX_TAIL, rx_tail))) {
		rc = CO_RC(ERROR);
		goto out;
	}

	if (want > CO_NIO_TX_SIZE)
		want = CO_NIO_TX_SIZE;

	/*
	 * A window of raw ring bytes at an absolute position; the caller owns
	 * record parsing. Split at the wrap, because the ring is circular and
	 * the walk is linear.
	 */
	off   = start & (CO_NIO_TX_SIZE - 1);
	first = want < CO_NIO_TX_SIZE - off ? want : CO_NIO_TX_SIZE - off;

	if (first &&
	    !CO_OK(co_kload_read(manager, net_io_va + CO_NIO_TX + off,
				 data, first))) {
		rc = CO_RC(ERROR);
		goto out;
	}
	if (first < want &&
	    !CO_OK(co_kload_read(manager, net_io_va + CO_NIO_TX,
				 data + first, want - first))) {
		rc = CO_RC(ERROR);
		goto out;
	}

	*size = want;

out:
	co_os_mutex_release(net_lock);
	return rc;
}
