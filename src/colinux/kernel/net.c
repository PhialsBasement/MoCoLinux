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
