/*
 * This source code is a part of coLinux source package.
 *
 * The code is licensed under the GPL. See the COPYING file at
 * the root directory.
 */

/*
 * The host side of the guest's network rings: fetch a snapshot, walk its
 * records, describe a frame.
 *
 * Extracted from elf_load.c when the slirp bridge became the second program
 * needing it. Nothing here talks to slirp or to the boot path; it is only the
 * ring format and the ioctls that move bytes across it.
 */

#include <string.h>

#include <colinux/common/common.h>
#include <colinux/os/alloc.h>
#include <colinux/os/user/misc.h>
#include <colinux/user/manager.h>

#include "conet_ring.h"

unsigned int co_net_le32(const unsigned char* p)
{
	return (unsigned int)p[0] | ((unsigned int)p[1] << 8) |
	       ((unsigned int)p[2] << 16) | ((unsigned int)p[3] << 24);
}

void co_net_describe_frame(const unsigned char* f, unsigned int len)
{
	unsigned int ethertype;

	if (len < 14) {
		co_terminal_print("      (runt: shorter than an ethernet header)\n");
		return;
	}

	ethertype = ((unsigned int)f[12] << 8) | f[13];
	co_terminal_print("      dst %02x:%02x:%02x:%02x:%02x:%02x"
			  "  src %02x:%02x:%02x:%02x:%02x:%02x  type 0x%04x\n",
			  f[0], f[1], f[2], f[3], f[4], f[5],
			  f[6], f[7], f[8], f[9], f[10], f[11], ethertype);

	if (ethertype == 0x0806 && len >= 42) {
		unsigned int op = ((unsigned int)f[20] << 8) | f[21];

		co_terminal_print("      ARP %s  sender %u.%u.%u.%u"
				  " (%02x:%02x:%02x:%02x:%02x:%02x)"
				  "  target %u.%u.%u.%u\n",
				  op == 1 ? "who-has" : op == 2 ? "reply" : "op?",
				  f[28], f[29], f[30], f[31],
				  f[22], f[23], f[24], f[25], f[26], f[27],
				  f[38], f[39], f[40], f[41]);
	} else if (ethertype == 0x86dd) {
		co_terminal_print("      IPv6%s\n",
				  (len >= 54 && f[20] == 58) ? " (ICMPv6)" : "");
	} else if (ethertype == 0x0800) {
		co_terminal_print("      IPv4\n");
	}
}

co_rc_t co_net_fetch(co_manager_handle_t handle,
		     unsigned int* tx_head, unsigned int* tx_tail,
		     unsigned int* rx_head, unsigned int* rx_tail,
		     unsigned char* ring)
{
	unsigned int h2, t2, r2h, r2t;
	unsigned int used, done, got;
	co_rc_t rc;

	/*
	 * Only tail..head is read below, and the walk touches nothing outside
	 * it -- but a zeroed buffer makes a parser bug deterministic rather
	 * than a function of whatever the heap last held.
	 */
	memset(ring, 0, CO_NETIO_TX_SIZE);

	got = 0;
	rc = co_manager_conet_dump(handle, tx_head, tx_tail, rx_head, rx_tail,
				   0, ring, &got);
	if (!CO_OK(rc))
		return rc;

	used = *tx_head - *tx_tail;
	if (used == 0 || used > CO_NETIO_TX_SIZE)
		return CO_RC(OK);	/* nothing, or a state the caller rejects */

	for (done = 0; done < used; done += got) {
		unsigned int pos  = *tx_tail + done;
		unsigned int off  = pos & (CO_NETIO_TX_SIZE - 1);
		unsigned int room = CO_NETIO_TX_SIZE - off;

		got = used - done;
		if (got > 8192)
			got = 8192;
		if (got > room)			/* never straddle the wrap */
			got = room;

		rc = co_manager_conet_dump(handle, &h2, &t2, &r2h, &r2t,
					   pos, ring + off, &got);
		if (!CO_OK(rc))
			return rc;
		if (got == 0)
			return CO_RC(ERROR);
	}

	return CO_RC(OK);
}

int co_net_walk(const unsigned char* ring,
		unsigned int tx_head, unsigned int tx_tail,
		co_net_frame_func_t func, void* data,
		unsigned int* consumed_out)
{
	unsigned int pos;
	int frames = 0;

	if (consumed_out)
		*consumed_out = tx_tail;

	if (tx_head - tx_tail > CO_NETIO_TX_SIZE)
		return -1;

	for (pos = tx_tail; pos != tx_head; ) {
		unsigned int off = pos & (CO_NETIO_TX_SIZE - 1);
		unsigned int len = co_net_le32(ring + off);
		unsigned int record = 4 + ((len + 3) & ~3u);
		unsigned char frame[CO_NETIO_MAX_FRAME];
		unsigned int first;

		if (len == 0 || len > CO_NETIO_MAX_FRAME ||
		    record > tx_head - pos)
			return -1;

		off = (off + 4) & (CO_NETIO_TX_SIZE - 1);
		first = len < CO_NETIO_TX_SIZE - off ? len : CO_NETIO_TX_SIZE - off;
		memcpy(frame, ring + off, first);
		if (first < len)
			memcpy(frame + first, ring, len - first);

		if (func && !func(data, frame, len))
			break;		/* left unconsumed, offered again */

		pos += record;
		frames++;
		if (consumed_out)
			*consumed_out = pos;
	}

	return frames;
}
