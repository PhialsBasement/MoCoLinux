/*
 * This source code is a part of coLinux source package.
 *
 * Dan Aloni <da-x@colinux.org>, 2003 (c)
 *
 * The code is licensed under the GPL. See the COPYING file at
 * the root directory.
 *
 */

#include <linux/compiler.h>

/*
 * Prevent some warning from an header inline function that
 * is not under __KERNEL__.
 */
#define unlikely

#include <linux/elf.h>

#include "daemon.h"
#include "conet_ring.h"
#include <stdlib.h>
#include <string.h>

#include <colinux/os/alloc.h>
#include <colinux/os/user/file.h>
#include <colinux/os/user/misc.h>
#include <colinux/os/user/manager.h>
#include <colinux/user/manager.h>

/*
 * ELF32 or ELF64 by target architecture.
 *
 * The two layouts are not merely differently sized -- Elf64_Sym reorders its
 * fields relative to co_elf_sym_t -- so this cannot be done by widening a few
 * types. Everything that touches a header goes through these typedefs, and the
 * class and machine are checked at load time so an image built for the wrong
 * architecture is rejected rather than silently misparsed.
 */
#if defined(__x86_64__)
typedef Elf64_Ehdr co_elf_ehdr_t;
typedef Elf64_Shdr co_elf_shdr_t;
typedef Elf64_Phdr co_elf_phdr_t;
typedef Elf64_Sym  co_elf_sym_t;
# define CO_ELF_EXPECTED_CLASS	 ELFCLASS64
# define CO_ELF_EXPECTED_MACHINE EM_X86_64
#else
typedef Elf32_Ehdr co_elf_ehdr_t;
typedef Elf32_Shdr co_elf_shdr_t;
typedef Elf32_Phdr co_elf_phdr_t;
typedef Elf32_Sym  co_elf_sym_t;
# define CO_ELF_EXPECTED_CLASS	 ELFCLASS32
# define CO_ELF_EXPECTED_MACHINE EM_386
#endif

struct co_elf_data {
	/* ELF binary buffer */
	unsigned char *buffer;
	unsigned long size;

	/* Elf header and seconds */
	co_elf_ehdr_t *header;
	co_elf_shdr_t *section_string_table_section;
	co_elf_shdr_t *string_table_section;
	co_elf_shdr_t *symbol_table_section;
};

struct co_elf_symbol {
	co_elf_sym_t sym;
};

/*
 * This code in this file basically allows to enumerate loadable
 * sections of the given ELF image file. The sections that interest
 * us most in the vmlinux are the sections that are actually
 * loadable (or allocatable, like the bss).
 *
 * Previously, I used to allocate each section in the kernel
 * separately when it was loaded by the ioctl below. But now, I just
 * figure out the entire size of the loaded kernel by looking at
 * the addresses of the start and end symbols, allocating the whole
 * kernel in a big chunk, and then use memset and memcpy in the
 * per section ioctl handler in order to initialize the loaded image.
 *
 * Optionally, it is possible to do this in a completely different
 * way: load the entire vmlinux file to kernel memory and then do
 * all the processing there (i.e, map the physical pages of the
 * loaded vmlinux memory according the section headers, initialize
 * the bss, get rid of unused sections, etc.).
 */
co_elf_phdr_t *co_get_program_header(co_elf_data_t *pl, long index)
{
	return (co_elf_phdr_t *)(pl->buffer + pl->header->e_phoff +
			      (pl->header->e_phentsize * index));
}

co_elf_off_t co_get_program_count(co_elf_data_t *pl)
{
	return pl->header->e_phnum;
}

co_elf_shdr_t *co_get_section_header(co_elf_data_t *pl, long index)
{
	return (co_elf_shdr_t *)(pl->buffer + pl->header->e_shoff +
			      (pl->header->e_shentsize * (index)));
}

co_elf_off_t co_get_section_count(co_elf_data_t *pl)
{
	return pl->header->e_shnum;
}

static void *co_get_at_offset(co_elf_data_t *pl, co_elf_shdr_t *section, co_elf_off_t index)
{
	return &pl->buffer[section->sh_offset + index];
}

char *co_get_section_name(co_elf_data_t *pl, co_elf_shdr_t *section)
{
	return co_get_at_offset(pl, pl->section_string_table_section, section->sh_name);
}

co_elf_shdr_t *co_get_section_by_name(co_elf_data_t *pl, const char *name)
{
	co_elf_off_t index;
	co_elf_shdr_t *section;

	for (index=1; index < co_get_section_count(pl); index++) {
		section = co_get_section_header(pl, index);

		if (strcmp(co_get_section_name(pl, section), name) == 0)
			return section;
	}
	return NULL;
}

co_elf_sym_t *co_get_symbol(co_elf_data_t *pl, co_elf_off_t index)
{
	return (co_elf_sym_t *)
		co_get_at_offset(pl,
				    pl->symbol_table_section, index*sizeof(co_elf_sym_t));
}

char *co_get_string(co_elf_data_t *pl, co_elf_off_t index)
{
	return co_get_at_offset(pl, pl->string_table_section, index);
}

co_elf_symbol_t *co_get_symbol_by_name(co_elf_data_t *pl, const char *name)
{
	co_elf_off_t index;
	co_elf_off_t symbols;

	symbols = pl->symbol_table_section->sh_size / sizeof(co_elf_sym_t);

	for (index=0; index < symbols; index++) {
		co_elf_sym_t *symbol = co_get_symbol(pl, index);

		if (strcmp(name, co_get_string(pl, symbol->st_name)) == 0)
			return (co_elf_symbol_t *)symbol;
	}

	return NULL;
}

co_elf_addr_t co_get_symbol_offset(co_elf_data_t *pl, co_elf_sym_t *symbol)
{
	/* It doesn't work with absolute symbols, need to fix that? */

	co_elf_shdr_t *section;

	section = co_get_section_header(pl, symbol->st_shndx);

	return symbol->st_value - section->sh_addr;
}

void *co_elf_get_symbol_data(co_elf_data_t *pl, co_elf_symbol_t *symbol)
{
	co_elf_shdr_t *section;

	section = co_get_section_header(pl, symbol->sym.st_shndx);

	return pl->buffer + symbol->sym.st_value - section->sh_addr + section->sh_offset;
}

co_elf_addr_t co_elf_get_symbol_value(co_elf_symbol_t *symbol)
{
 	return symbol->sym.st_value;
}

co_rc_t co_elf_image_read(co_elf_data_t **pl_out, void *elf_buf, unsigned long size)
{
	co_elf_data_t *pl;

	pl = co_os_malloc(sizeof(co_elf_data_t));
	if (!pl)
		return CO_RC(OUT_OF_MEMORY);

	*pl_out = pl;

	pl->header = (co_elf_ehdr_t *)elf_buf;
	pl->buffer = elf_buf;
	pl->size = size;

	if (memcmp(pl->header->e_ident, "\x7F""ELF", 4))
		return CO_RC(ERROR_READING_VMLINUX_FILE);

	pl->section_string_table_section =\
		co_get_section_header(pl, pl->header->e_shstrndx);

	if (pl->section_string_table_section == NULL)
		return CO_RC(ERROR);

	pl->string_table_section = co_get_section_by_name(pl, ".strtab");
	if (pl->string_table_section == NULL)
		return CO_RC(ERROR);

	pl->symbol_table_section = co_get_section_by_name(pl, ".symtab");
	if (pl->symbol_table_section == NULL)
		return CO_RC(ERROR);

	return CO_RC(OK);
}

co_rc_t co_section_load(co_daemon_t *daemon, unsigned long index)
{
	co_elf_shdr_t *section;
	co_monitor_ioctl_load_section_t params;
	co_rc_t rc;

	section = co_get_section_header(daemon->elf_data, index);
	if (section->sh_flags & SHF_ALLOC) {
		if (section->sh_type == SHT_NOBITS)
			params.user_ptr = NULL;
		else
			params.user_ptr = co_get_at_offset(daemon->elf_data, section, 0);

		params.address = section->sh_addr;
		params.size = section->sh_size;
		params.index = index;

		/*
		 * Load each ELF section to kernel space separately.
		 */
		rc = co_user_monitor_load_section(daemon->monitor, &params);
		if (!CO_OK(rc))
			return rc;
	}

	return CO_RC(OK);
}

/*
 * Load all ELF sections to host OS kernel memory.
 */
co_rc_t co_elf_image_load(co_daemon_t *daemon)
{
	co_elf_off_t index;
	co_elf_off_t sections;
	co_rc_t rc;

	sections = co_get_section_count(daemon->elf_data);

	for (index=1; index < sections; index++) {
		rc = co_section_load(daemon, index);

		if (!CO_OK(rc))
			return rc;
	}

	return CO_RC(OK);
}

/*
 * Read an image and print what the parser makes of it.
 *
 * Deliberately prints every address as a full 64-bit quantity so a truncation
 * shows up as visibly wrong rather than plausibly small -- a kernel symbol that
 * reads 0x81000000 instead of 0xffffffff81000000 is the whole class of bug this
 * exists to catch.
 */
co_rc_t co_elf_dump(const char *filename)
{
	co_elf_data_t *pl;
	co_elf_off_t index, sections;
	co_rc_t rc;
	char *buf;
	unsigned long size;
	static const char *wanted[] = {
		"_text", "startup_64", "x86_64_start_kernel", "start_kernel",
		"init_top_pgt", "early_top_pgt", "boot_params",
		"__bss_start", "__bss_stop", "_end", NULL
	};
	int i;

	rc = co_os_file_load(filename, &buf, &size, 0);
	if (!CO_OK(rc)) {
		co_terminal_print("cannot read %s\n", filename);
		return rc;
	}

	co_terminal_print("%s: %lu bytes\n\n", filename, size);

	rc = co_elf_image_read(&pl, buf, size);
	if (!CO_OK(rc)) {
		co_terminal_print("not a usable ELF image for this build (rc %x)\n", (int)rc);
		co_os_file_free(buf);
		return rc;
	}

	co_terminal_print("  class %d  machine %d  entry 0x%016llx\n",
			  pl->header->e_ident[EI_CLASS], pl->header->e_machine,
			  (unsigned long long)pl->header->e_entry);
	co_terminal_print("  %llu sections, %llu program headers\n\n",
			  (unsigned long long)co_get_section_count(pl),
			  (unsigned long long)co_get_program_count(pl));

	co_terminal_print("  allocatable sections (these are what get loaded):\n");
	sections = co_get_section_count(pl);
	for (index = 1; index < sections; index++) {
		co_elf_shdr_t *section = co_get_section_header(pl, index);

		if (!(section->sh_flags & SHF_ALLOC))
			continue;

		co_terminal_print("    %-20s addr 0x%016llx  size 0x%08llx%s\n",
				  co_get_section_name(pl, section),
				  (unsigned long long)section->sh_addr,
				  (unsigned long long)section->sh_size,
				  (section->sh_type == SHT_NOBITS) ? "  (nobits)" : "");
	}

	co_terminal_print("\n  symbols the loader depends on:\n");
	for (i = 0; wanted[i]; i++) {
		co_elf_symbol_t *sym = co_get_symbol_by_name(pl, wanted[i]);

		if (sym)
			co_terminal_print("    %-24s 0x%016llx\n", wanted[i],
					  (unsigned long long)co_elf_get_symbol_value(sym));
		else
			co_terminal_print("    %-24s NOT FOUND\n", wanted[i]);
	}

	co_os_file_free(buf);
	return CO_RC(OK);
}

/*
 * Read the guest's network rings out of guest memory, after the run.
 *
 * The guest driver (drivers/net/conet_colinux.c) keeps two byte rings in
 * co_colinux_net_io and the layout is ABI: four u32 indices at +0, the TX
 * ring at +0x10, the RX ring after it. Records are a 32-bit length, the
 * frame, padding to a four-byte boundary; every record starts aligned so the
 * length word never straddles the wrap.
 *
 * This runs where the printk-ring decoder runs: in the boot daemon, after the
 * run, before KLOAD_END. That placement is the safety argument, not a
 * convenience. CO_MANAGER_IOCTL_KREAD takes no lock against teardown -- its
 * only caller has always been this process, sequenced before its own
 * KLOAD_END, and a *cross-process* reader could race the space being freed
 * and walk freed page tables, which in a driver is a bugcheck. So the rings
 * are decoded here, on the same thread, where the race cannot be expressed.
 * A live reader is R3's business, and it arrives together with the lock that
 * makes it legal.
 *
 * Offsets are numbers here rather than a shared struct, for the reason the
 * printk decoder gives: this is a Win64 program parsing an LP64 kernel's
 * memory, and the two compilers must not be allowed to disagree.
 */
/*
 * Parse and print a snapshot of the rings: the four indices and, when the TX
 * ring holds anything, a full-size copy of its byte array indexed by the
 * same masks the guest uses. One parser for both viewers -- the post-run
 * dump (bytes via KREAD) and --net-dump (bytes via the CONET_DUMP ioctl) --
 * because a decoder that exists twice is a decoder that disagrees with
 * itself the first time only one copy is fixed.
 */
static int co_net_print_rings(unsigned int tx_head, unsigned int tx_tail,
			      unsigned int rx_head, unsigned int rx_tail,
			      const unsigned char* ring)
{
	unsigned int used, pos, frames = 0;

	co_terminal_print("  tx head %u  tail %u  (%u bytes in the ring)\n",
			  tx_head, tx_tail, tx_head - tx_tail);
	co_terminal_print("  rx head %u  tail %u  (%u bytes in the ring)\n",
			  rx_head, rx_tail, rx_head - rx_tail);

	used = tx_head - tx_tail;
	if (used > CO_NETIO_TX_SIZE) {
		co_terminal_print("  tx indices are not a ring state -- desynchronised\n");
		return -1;
	}
	if (used == 0)
		goto rx_check;

	for (pos = tx_tail; pos != tx_head; frames++) {
		unsigned int off = pos & (CO_NETIO_TX_SIZE - 1);
		unsigned int len = co_net_le32(ring + off);
		unsigned int record = 4 + ((len + 3) & ~3u);
		unsigned int i, first;
		unsigned char frame[CO_NETIO_MAX_FRAME];

		if (len == 0 || len > CO_NETIO_MAX_FRAME ||
		    record > tx_head - pos) {
			co_terminal_print("  RECORD %u at +%u IS NOT A FRAME: len %u"
					  " with %u bytes left -- ring corrupt\n",
					  frames, off, len, tx_head - pos);
			return -1;
		}

		off = (off + 4) & (CO_NETIO_TX_SIZE - 1);
		first = len < CO_NETIO_TX_SIZE - off ? len : CO_NETIO_TX_SIZE - off;
		memcpy(frame, ring + off, first);
		if (first < len)
			memcpy(frame + first, ring, len - first);

		co_terminal_print("\n  frame %u, %u bytes:\n", frames, len);
		for (i = 0; i < len; i += 16) {
			unsigned int j, n = (len - i < 16) ? len - i : 16;
			char line[3 * 16 + 1];

			for (j = 0; j < n; j++)
				co_snprintf(line + 3 * j, 4, "%02x ", frame[i + j]);
			co_terminal_print("      %s\n", line);
		}
		co_net_describe_frame(frame, len);

		pos += record;
	}

	co_terminal_print("\n  %u frames, %u bytes, and they account for the ring"
			  " exactly: pos == tx_head\n", frames, used);

rx_check:
	if (rx_head == 0 && rx_tail == 0)
		co_terminal_print("  rx ring untouched (head == tail == 0),"
				  " as it must be while no host writes it\n");
	else
		co_terminal_print("  RX RING NOT PRISTINE -- nothing should have"
				  " written it yet\n");
	return 0;
}

static void co_dump_net_rings(co_manager_handle_t handle, co_elf_data_t* pl)
{
	co_elf_symbol_t* s_io = co_get_symbol_by_name(pl, "co_colinux_net_io");
	unsigned long long io_va;
	unsigned char hdr[16];
	unsigned char* ring = NULL;
	unsigned int tx_head, tx_tail, rx_head, rx_tail;
	co_rc_t rc;

	if (!s_io)
		return;		/* a kernel without the conet driver */
	io_va = co_elf_get_symbol_value(s_io);

	rc = co_manager_kread(handle, io_va, hdr, sizeof(hdr));
	if (!CO_OK(rc)) {
		co_terminal_print("  net rings unreadable at 0x%016llx (rc %x)\n",
				  io_va, (int)rc);
		return;
	}

	tx_head = co_net_le32(hdr + CO_NETIO_TX_HEAD);
	tx_tail = co_net_le32(hdr + CO_NETIO_TX_TAIL);
	rx_head = co_net_le32(hdr + CO_NETIO_RX_HEAD);
	rx_tail = co_net_le32(hdr + CO_NETIO_RX_TAIL);

	co_terminal_print("  co_colinux_net_io at 0x%016llx\n", io_va);

	if (tx_head != tx_tail && tx_head - tx_tail <= CO_NETIO_TX_SIZE) {
		unsigned int chunk = 4096, off;

		ring = (unsigned char*)co_os_malloc(CO_NETIO_TX_SIZE);
		if (!ring)
			return;

		/*
		 * The whole TX array, in pieces the ioctl's staging buffer
		 * keeps small. Records wrap; masks are cheaper to apply to a
		 * local copy than to scatter across reads.
		 */
		for (off = 0; off < CO_NETIO_TX_SIZE; off += chunk) {
			rc = co_manager_kread(handle, io_va + CO_NETIO_TX + off,
					      ring + off, chunk);
			if (!CO_OK(rc)) {
				co_terminal_print("  tx ring read failed at +%u (rc %x)\n",
						  off, (int)rc);
				co_os_free(ring);
				return;
			}
		}
	}

	co_net_print_rings(tx_head, tx_tail, rx_head, rx_tail, ring);

	if (ring)
		co_os_free(ring);
}

/*
 * --net-dump: the same view, live, from a second process.
 *
 * The bytes come through CO_MANAGER_IOCTL_CONET_DUMP rather than KREAD,
 * because KREAD's walk is unprotected against the run's teardown and this
 * caller is exactly the cross-process reader that could race it. The CONET
 * ioctl walks under the net lock and answers NOT_FOUND once the address is
 * retired, so a dump racing the run's end fails politely instead of walking
 * freed page tables.
 *
 * The indices come from the first call and the bytes from as many calls as
 * the window needs. That snapshot stays consistent without a lock across
 * calls: the guest only appends, the host never consumes here (tx_tail does
 * not move in this rung), and a full ring drops rather than overwrites -- so
 * every byte in the snapshot's tail..head range is immutable once published.
 */
co_rc_t co_elf_net_dump_live(void)
{
	co_manager_handle_t handle;
	unsigned char* ring;
	unsigned int tx_head = 0, tx_tail = 0, rx_head = 0, rx_tail = 0;
	co_rc_t rc;

	handle = co_os_manager_open();
	if (!handle) {
		co_terminal_print("net-dump: cannot open the driver -- is it loaded?\n");
		return CO_RC(ERROR);
	}

	ring = (unsigned char*)co_os_malloc(CO_NETIO_TX_SIZE);
	if (!ring) {
		co_os_manager_close(handle);
		return CO_RC(OUT_OF_MEMORY);
	}

	rc = co_net_fetch(handle, &tx_head, &tx_tail, &rx_head, &rx_tail, ring);
	if (!CO_OK(rc)) {
		co_terminal_print("net-dump: no live guest with net rings (rc %x)\n",
				  (int)rc);
		goto out;
	}

	co_terminal_print("the guest's network rings, live:\n");
	co_net_print_rings(tx_head, tx_tail, rx_head, rx_tail, ring);
	rc = CO_RC(OK);

out:
	co_os_free(ring);
	co_os_manager_close(handle);
	return rc;
}

/*
 * --net-take: print the TX ring like --net-dump, then consume it -- advance
 * tx_tail to the head of the snapshot just printed, through the validated
 * TAKE ioctl. Only a ring that parsed clean is consumed; a corrupt ring is
 * left exactly as found, because consuming what could not be decoded
 * destroys the evidence.
 *
 * An explicit NEWTAIL argument bypasses the parse and asks the driver for
 * exactly that value. It exists to test the driver's validation from the
 * command line -- a bogus tail must come back refused, with nothing written.
 */
/*
 * A peer on the other end of the wire.
 *
 * Everything below this point is a stand-in for slirp: enough of a host to
 * answer the two things a guest says when it is told it has a network, so
 * that both ring directions can be exercised with real kernel involvement on
 * the guest side -- netif_rx delivering into the IP stack, and ping seeing
 * replies -- before a line of 2004-era NAT code is allowed to run.
 *
 * The peer is 10.0.2.2 at 02:c0:11:00:00:02, which is where slirp's gateway
 * will be, so the guest's configuration does not change when slirp replaces
 * this.
 */
static const unsigned char co_peer_mac[6] = { 0x02, 0xc0, 0x11, 0x00, 0x00, 0x02 };
static const unsigned char co_peer_ip[4]  = { 10, 0, 2, 2 };

static unsigned int co_net_ip_csum(const unsigned char* p, unsigned int len)
{
	unsigned long sum = 0;

	while (len > 1) {
		sum += ((unsigned long)p[0] << 8) | p[1];
		p += 2;
		len -= 2;
	}
	if (len)
		sum += (unsigned long)p[0] << 8;

	while (sum >> 16)
		sum = (sum & 0xffff) + (sum >> 16);

	return (unsigned int)(~sum & 0xffff);
}

/*
 * Build a reply to one frame, or return 0 if this peer has nothing to say
 * about it. ARP requests for our address get an answer; ICMP echo requests
 * to it get an echo reply. Everything else -- the guest's IPv6 chatter, in
 * practice -- is ignored exactly as an absent host would.
 */
static unsigned int co_net_peer_reply(const unsigned char* f, unsigned int len,
				      unsigned char* out)
{
	unsigned int ethertype;

	if (len < 14)
		return 0;

	ethertype = ((unsigned int)f[12] << 8) | f[13];

	if (ethertype == 0x0806 && len >= 42) {
		unsigned int op = ((unsigned int)f[20] << 8) | f[21];

		if (op != 1 || memcmp(f + 38, co_peer_ip, 4) != 0)
			return 0;

		memcpy(out + 0, f + 6, 6);		/* to the asker	     */
		memcpy(out + 6, co_peer_mac, 6);
		out[12] = 0x08; out[13] = 0x06;
		out[14] = 0x00; out[15] = 0x01;		/* ethernet	     */
		out[16] = 0x08; out[17] = 0x00;		/* IPv4		     */
		out[18] = 6;    out[19] = 4;
		out[20] = 0x00; out[21] = 0x02;		/* reply	     */
		memcpy(out + 22, co_peer_mac, 6);	/* sender = us	     */
		memcpy(out + 28, co_peer_ip, 4);
		memcpy(out + 32, f + 22, 6);		/* target = asker    */
		memcpy(out + 38, f + 28, 4);
		return 42;
	}

	if (ethertype == 0x0800 && len >= 34) {
		unsigned int ihl = (f[14] & 0x0f) * 4;
		unsigned int icmp = 14 + ihl;

		if (f[23] != 1 || ihl < 20 || len < icmp + 8)
			return 0;				/* not ICMP	     */
		if (memcmp(f + 30, co_peer_ip, 4) != 0)
			return 0;				/* not to us	     */
		if (f[icmp] != 8)
			return 0;				/* not an echo	     */

		memcpy(out, f, len);
		memcpy(out + 0, f + 6, 6);
		memcpy(out + 6, co_peer_mac, 6);
		memcpy(out + 26, f + 30, 4);			/* src = us	     */
		memcpy(out + 30, f + 26, 4);			/* dst = the guest   */
		out[icmp] = 0;					/* echo reply	     */

		/*
		 * The IP header checksum is unchanged: swapping the two
		 * addresses adds the same words in a different order. The ICMP
		 * checksum is recomputed rather than adjusted, because getting
		 * an incremental update wrong produces a reply the guest
		 * silently discards, which looks exactly like the ring not
		 * working.
		 */
		out[icmp + 2] = 0;
		out[icmp + 3] = 0;
		{
			unsigned int c = co_net_ip_csum(out + icmp, len - icmp);

			out[icmp + 2] = (unsigned char)(c >> 8);
			out[icmp + 3] = (unsigned char)(c & 0xff);
		}
		return len;
	}

	return 0;
}

co_rc_t co_elf_net_peer_live(const char* seconds_arg)
{
	co_manager_handle_t handle;
	unsigned char* ring;
	unsigned int tx_head = 0, tx_tail = 0, rx_head = 0, rx_tail = 0;
	unsigned int seen = 0, replied = 0, full = 0, rounds = 0;
	unsigned long deadline_ms, waited_ms = 0;
	co_rc_t rc;

	deadline_ms = seconds_arg && seconds_arg[0]
		    ? strtoul(seconds_arg, NULL, 0) * 1000 : 60000;

	handle = co_os_manager_open();
	if (!handle) {
		co_terminal_print("net-peer: cannot open the driver -- is it loaded?\n");
		return CO_RC(ERROR);
	}

	ring = (unsigned char*)co_os_malloc(CO_NETIO_TX_SIZE);
	if (!ring) {
		co_os_manager_close(handle);
		return CO_RC(OUT_OF_MEMORY);
	}

	co_terminal_print("net-peer: answering ARP and ICMP echo for 10.0.2.2"
			  " (%02x:%02x:%02x:%02x:%02x:%02x) for %lu ms\n",
			  co_peer_mac[0], co_peer_mac[1], co_peer_mac[2],
			  co_peer_mac[3], co_peer_mac[4], co_peer_mac[5],
			  deadline_ms);

	while (waited_ms < deadline_ms) {
		unsigned int pos, took = 0;

		rc = co_net_fetch(handle, &tx_head, &tx_tail,
				       &rx_head, &rx_tail, ring);
		if (!CO_OK(rc)) {
			co_terminal_print("net-peer: the guest's rings went away"
					  " (rc %x) after %u frames\n", (int)rc, seen);
			break;
		}

		rounds++;

		if (tx_head == tx_tail || tx_head - tx_tail > CO_NETIO_TX_SIZE) {
			co_os_user_msleep(2);
			waited_ms += 2;
			continue;
		}

		for (pos = tx_tail; pos != tx_head; ) {
			unsigned int off = pos & (CO_NETIO_TX_SIZE - 1);
			unsigned int flen = co_net_le32(ring + off);
			unsigned int record = 4 + ((flen + 3) & ~3u);
			unsigned char frame[CO_NETIO_MAX_FRAME];
			unsigned char reply[CO_NETIO_MAX_FRAME];
			unsigned char one_record[4 + CO_NETIO_MAX_FRAME + 3];
			unsigned int first, rlen;

			if (flen == 0 || flen > CO_NETIO_MAX_FRAME ||
			    record > tx_head - pos) {
				co_terminal_print("net-peer: ring corrupt at +%u"
						  " (len %u) -- stopping\n", off, flen);
				goto done;
			}

			off = (off + 4) & (CO_NETIO_TX_SIZE - 1);
			first = flen < CO_NETIO_TX_SIZE - off
			      ? flen : CO_NETIO_TX_SIZE - off;
			memcpy(frame, ring + off, first);
			if (first < flen)
				memcpy(frame + first, ring, flen - first);

			seen++;
			pos += record;
			took = pos;

			rlen = co_net_peer_reply(frame, flen, reply);
			if (!rlen)
				continue;

			/*
			 * One frame, in the batch format the ioctl now takes:
			 * a length word, the frame, padding to four bytes.
			 * --net-peer answers a packet at a time by design --
			 * it stands in for the far end of a wire, not for a
			 * bulk sender -- so there is nothing here to coalesce.
			 */
			{
				unsigned int rec = 4 + ((rlen + 3) & ~3u);
				unsigned int put = 0;

				memset(one_record, 0, rec);
				memcpy(one_record, &rlen, 4);
				memcpy(one_record + 4, reply, rlen);

				rc = co_manager_conet_put(handle, one_record,
							  rec, 1, &put);
				if (CO_OK(rc) && put != 1)
					rc = CO_RC(OUT_OF_MEMORY);
			}
			if (CO_OK(rc)) {
				replied++;
			} else {
				/*
				 * A full RX ring is the guest not draining
				 * fast enough, not an error. Stop consuming
				 * here so the frame that produced this reply
				 * is offered again next round.
				 */
				full++;
				took = pos - record;
				break;
			}
		}

		if (took != tx_tail) {
			unsigned int h, t, rh, rt;

			rc = co_manager_conet_take(handle, took, &h, &t, &rh, &rt);
			if (!CO_OK(rc)) {
				co_terminal_print("net-peer: consume refused"
						  " (rc %x)\n", (int)rc);
				break;
			}
		}
	}

done:
	co_terminal_print("net-peer: %u frames seen, %u answered, %u deferred"
			  " on a full rx ring, %u polls\n",
			  seen, replied, full, rounds);

	co_os_free(ring);
	co_os_manager_close(handle);
	return CO_RC(OK);
}

co_rc_t co_elf_net_take_live(const char* new_tail_arg)
{
	co_manager_handle_t handle;
	unsigned char* ring;
	unsigned int tx_head = 0, tx_tail = 0, rx_head = 0, rx_tail = 0;
	co_rc_t rc;

	handle = co_os_manager_open();
	if (!handle) {
		co_terminal_print("net-take: cannot open the driver -- is it loaded?\n");
		return CO_RC(ERROR);
	}

	if (new_tail_arg && new_tail_arg[0]) {
		unsigned int asked = (unsigned int)strtoul(new_tail_arg, NULL, 0);

		rc = co_manager_conet_take(handle, asked,
					   &tx_head, &tx_tail, &rx_head, &rx_tail);
		if (CO_OK(rc))
			co_terminal_print("net-take: tail set to %u (head %u)\n",
					  tx_tail, tx_head);
		else
			co_terminal_print("net-take: REFUSED tail %u (rc %x),"
					  " nothing written\n", asked, (int)rc);
		co_os_manager_close(handle);
		return rc;
	}

	ring = (unsigned char*)co_os_malloc(CO_NETIO_TX_SIZE);
	if (!ring) {
		co_os_manager_close(handle);
		return CO_RC(OUT_OF_MEMORY);
	}

	rc = co_net_fetch(handle, &tx_head, &tx_tail, &rx_head, &rx_tail, ring);
	if (!CO_OK(rc)) {
		co_terminal_print("net-take: no live guest with net rings (rc %x)\n",
				  (int)rc);
		goto out;
	}

	co_terminal_print("the guest's network rings, live:\n");
	if (co_net_print_rings(tx_head, tx_tail, rx_head, rx_tail, ring) != 0) {
		co_terminal_print("net-take: NOT consuming a ring that did not parse\n");
		rc = CO_RC(ERROR);
		goto out;
	}

	if (tx_head == tx_tail) {
		co_terminal_print("net-take: nothing to consume\n");
		rc = CO_RC(OK);
		goto out;
	}

	{
		unsigned int old_tail = tx_tail, want = tx_head - tx_tail;

		rc = co_manager_conet_take(handle, tx_head,
					   &tx_head, &tx_tail, &rx_head, &rx_tail);
		if (CO_OK(rc))
			co_terminal_print("net-take: consumed %u bytes -- tail"
					  " %u -> %u, ring now holds %u\n",
					  want, old_tail, tx_tail,
					  tx_head - tx_tail);
		else
			co_terminal_print("net-take: consume refused or failed"
					  " (rc %x)\n", (int)rc);
	}

out:
	co_os_free(ring);
	co_os_manager_close(handle);
	return rc;
}

/*
 * Load a kernel image into a guest address space in the driver, then enter it.
 *
 * The sections go over in chunks rather than whole: a section can be megabytes
 * and the ioctl buffer is copied for every call, so streaming keeps the peak
 * allocation bounded and the failure, if there is one, attributable to a
 * particular range rather than to "the kernel".
 *
 * SHT_NOBITS sections -- .bss and .brk, about a megabyte and a half here -- are
 * sent as zeroing chunks. They have no file content but they do need pages, and
 * a guest that finds its .bss unmapped fails in ways that look nothing like the
 * cause.
 */
#define CO_KLOAD_CHUNK	0x8000

/*
 * How much physical memory the guest is told it has, in megabytes, when --mem
 * does not say otherwise.
 *
 * 1 GB, and the way the 2 GB attempt failed is the reason this is a flag now
 * rather than a constant.
 *
 * The host had 2.4 GB free and the box still became unresponsive the instant
 * the daemon started, before the guest executed an instruction -- because free
 * memory is not the resource being asked for. co_kload_build_ram wants unbroken
 * 32 MB physical runs from MmAllocateContiguousMemory, and after a session with
 * a browser and a package manager, free memory is holes. Windows then trims
 * working sets and repurposes standby pages trying to manufacture runs that do
 * not exist, with the memory manager's locks held, and the machine stops
 * answering while it tries. Task Manager shows memory available throughout,
 * which is what makes this so easy to misdiagnose -- and I misdiagnosed it as
 * a total-memory problem first.
 *
 * The allocation is also non-pageable, so whatever the guest gets is taken out
 * of the host's working set permanently for the life of the run, not shared
 * with it.
 *
 * 1 GB has booted this box repeatedly. Whether more works depends on how
 * fragmented that particular machine is at that particular moment, which is
 * exactly the sort of thing that should be tried from a command line rather
 * than discovered after a cross-compile. Falling short remains reported and
 * non-fatal: the e820 describes what was obtained, so a guest that gets less
 * boots with less and says so.
 *
 * The ceiling is the block count rather than this number. Block 0 is image plus
 * page tables and every later block is at most CO_KLOAD_CHUNK_BYTES, so it is
 * 44 + 32 * (CO_KLOAD_MAX_BLOCKS - 1) MB, near 4 GB. None of that helps if the
 * host cannot produce the runs.
 *
 * The real fix is not a better number: it is pseudo-physical memory, which
 * removes the contiguity requirement altogether. See TODO.
 */
#define CO_GUEST_RAM_DEFAULT_MB	1024

/* One e820 entry: 8-byte address, 8-byte size, 4-byte type, packed to 20. */
static void co_e820_entry(unsigned char* p, unsigned long long addr,
			  unsigned long long size, unsigned int type)
{
	int i;

	for (i = 0; i < 8; i++)  p[i]      = (unsigned char)(addr >> (8 * i));
	for (i = 0; i < 8; i++)  p[8 + i]  = (unsigned char)(size >> (8 * i));
	for (i = 0; i < 4; i++)  p[16 + i] = (unsigned char)(type >> (8 * i));
}


/*
 * Dump the kernel's own log, read out of guest memory after a run.
 *
 * printk() does not write to consoles. It appends records to a ringbuffer --
 * kernel/printk/printk_ringbuffer.c -- and consoles drain that buffer later,
 * if any ever register and get the chance. Every boot so far has died before
 * that, which is why the runs were silent: the kernel was never quiet, nobody
 * was reading. The buffer is guest memory and the guest's tables are ours to
 * walk, so read it the way a crash dump reader would.
 *
 * Layout facts, taken from the 7.1.5 tree this kernel is built from and used
 * as offsets rather than shared structs, because this is a Win64 program
 * parsing an LP64 kernel's memory -- the compilers must not be allowed to
 * disagree about what a long is:
 *
 *   prb                        pointer to the live printk_ringbuffer
 *   printk_ringbuffer          desc_ring at +0, text_data_ring at +48
 *   desc_ring                  count_bits +0, descs +8, infos +16,
 *                              head_id +24, tail_id +32
 *   text_data_ring             size_bits +0, data +8
 *   prb_desc (24 bytes)        state_var +0, blk_lpos.begin +8, .next +16
 *   printk_info (88 bytes)     seq +0, ts_nsec +8, text_len +16
 *   state_var                  descriptor id | state << 62; committed = 1,
 *                              finalized = 2
 *   data block                 8-byte id, then the record text
 *   lpos                       low bit set means dataless; 0x3 both sides is
 *                              an empty line. A block whose begin and next
 *                              fall in different wraps of the ring starts at
 *                              index 0, not at begin's index.
 */
static void co_dump_kernel_log_ex(co_manager_handle_t handle, co_elf_data_t* pl,
				  unsigned long long* p_since, int summary)
{
	co_elf_symbol_t* s_prb = co_get_symbol_by_name(pl, "prb");
	unsigned long long rb_va = 0, descs_va, infos_va, data_va;
	unsigned char rb[80];
	unsigned int count_bits, size_bits;
	unsigned long long desc_count, data_size;
	unsigned long long head_id, tail_id, id;
	unsigned long long shown = 0, missed = 0;
	unsigned long long since = p_since ? *p_since : 0;
	unsigned long long high = since;	/* highest seq+1 printed */
	co_rc_t rc;

	if (s_prb)
		if (!CO_OK(co_manager_kread(handle, co_elf_get_symbol_value(s_prb),
					    &rb_va, 8)))
			rb_va = 0;
	if (!rb_va) {
		co_elf_symbol_t* s_static = co_get_symbol_by_name(pl, "printk_rb_static");

		if (!s_static) {
			if (summary)
				co_terminal_print("  (no prb / printk_rb_static symbol -- log unreadable)\n");
			return;
		}
		rb_va = co_elf_get_symbol_value(s_static);
	}

	rc = co_manager_kread(handle, rb_va, rb, sizeof(rb));
	if (!CO_OK(rc)) {
		if (summary)
			co_terminal_print("  (printk_ringbuffer at 0x%llx unreadable, rc %x)\n",
					  rb_va, (int)rc);
		return;
	}

	memcpy(&count_bits, rb + 0,  4);
	memcpy(&descs_va,   rb + 8,  8);
	memcpy(&infos_va,   rb + 16, 8);
	memcpy(&head_id,    rb + 24, 8);
	memcpy(&tail_id,    rb + 32, 8);
	memcpy(&size_bits,  rb + 48, 4);
	memcpy(&data_va,    rb + 56, 8);

	if (count_bits < 4 || count_bits > 20 || size_bits < 8 || size_bits > 26) {
		if (summary)
			co_terminal_print("  (ringbuffer geometry is nonsense: %u desc bits,"
					  " %u data bits -- wrong offsets or trampled memory)\n",
					  count_bits, size_bits);
		return;
	}

	desc_count = 1ULL << count_bits;
	data_size  = 1ULL << size_bits;

	for (id = tail_id; (long long)(head_id - id) >= 0; id++) {
		unsigned char desc[24], info[24];
		unsigned long long sv, begin, next, seq, ts;
		unsigned int state;
		unsigned short text_len;
		unsigned long long text_va, text_avail;
		char text[1024];

		if (id - tail_id > desc_count + 64) {
			if (summary)
				co_terminal_print("  (stopping: walked more ids than exist"
						  " -- corrupt head/tail)\n");
			break;
		}

		if (!CO_OK(co_manager_kread(handle,
					    descs_va + (id & (desc_count - 1)) * 24,
					    desc, sizeof(desc)))) {
			missed++;
			continue;
		}
		memcpy(&sv,    desc + 0,  8);
		memcpy(&begin, desc + 8,  8);
		memcpy(&next,  desc + 16, 8);

		state = (unsigned int)(sv >> 62) & 3;
		if ((sv & ~(3ULL << 62)) != id)
			continue;		/* recycled or never used */
		if (state != 1 && state != 2)
			continue;		/* not committed/finalized */

		if (!CO_OK(co_manager_kread(handle,
					    infos_va + (id & (desc_count - 1)) * 88,
					    info, sizeof(info)))) {
			missed++;
			continue;
		}
		memcpy(&seq,      info + 0,  8);
		memcpy(&ts,       info + 8,  8);
		memcpy(&text_len, info + 16, 2);

		/*
		 * Only what has not been streamed already. This is what makes
		 * a 400 ms poll from the live thread emit just the new lines
		 * each time instead of the whole log over and over.
		 */
		if (p_since && seq < since)
			continue;
		if (seq + 1 > high)
			high = seq + 1;

		if ((begin & 1) && (next & 1)) {
			if (begin == 0x3 && next == 0x3)
				co_terminal_print("  [%5llu.%06llu]\n",
						  ts / 1000000000ULL,
						  (ts % 1000000000ULL) / 1000);
			continue;
		}

		/* A block never straddles the wrap; a wrapping one sits at 0. */
		if ((begin >> size_bits) == (next >> size_bits)) {
			text_va    = data_va + (begin & (data_size - 1)) + 8;
			text_avail = (next & (data_size - 1))
				     - (begin & (data_size - 1)) - 8;
		} else if (((begin + data_size) >> size_bits) == (next >> size_bits)) {
			text_va    = data_va + 8;
			text_avail = (next & (data_size - 1)) - 8;
		} else {
			missed++;
			continue;
		}

		if (text_avail > text_len)
			text_avail = text_len;
		if (text_avail > sizeof(text) - 1)
			text_avail = sizeof(text) - 1;

		if (!CO_OK(co_manager_kread(handle, text_va, text,
					    (unsigned long)text_avail))) {
			missed++;
			continue;
		}
		text[text_avail] = 0;

		co_terminal_print("  [%5llu.%06llu] %s%s\n",
				  ts / 1000000000ULL,
				  (ts % 1000000000ULL) / 1000,
				  text,
				  (text_len > text_avail) ? "  (truncated)" : "");
		shown++;
	}

	if (p_since)
		*p_since = high;

	if (summary) {
		co_terminal_print("\n  %llu records", shown);
		if (missed)
			co_terminal_print(", %llu unreadable or torn", missed);
		co_terminal_print("  (descriptor ids %llu..%llu)\n", tail_id, head_id);
	}
}

static void co_dump_kernel_log(co_manager_handle_t handle, co_elf_data_t* pl)
{
	unsigned long long since = 0;

	co_dump_kernel_log_ex(handle, pl, &since, 1);
}

/*
 * The live kernel log: a thread that streams the printk ring while the boot
 * ioctl blocks the main thread inside the driver.
 *
 * This exists because kboot is one long blocking call -- the guest runs to a
 * stop, a wedge, or a bugcheck entirely inside it, and until it returns the
 * post-run dump cannot run. A guest that hangs mid-boot, or takes the host
 * down, then left nothing to read. This prints every record as it appears, so
 * the last thing the guest said is on disk the instant it says it, wedge or
 * not.
 *
 * It reads on a SECOND driver handle, concurrently with kboot on the first,
 * and the main thread joins it before KLOAD_END -- so its page-table walk is
 * sequenced before the guest's memory is freed, which is the same invariant
 * that makes the post-run dump safe and the thing a cross-process reader
 * cannot promise.
 */
struct co_klog_ctx {
	co_manager_handle_t handle;
	co_elf_data_t*	    pl;
	unsigned long long* since;
	volatile int	    stop;
};

static void co_klog_stream(void* arg)
{
	struct co_klog_ctx* c = arg;

	for (;;) {
		co_dump_kernel_log_ex(c->handle, c->pl, c->since, 0);
		if (c->stop)
			return;
		co_os_user_msleep(400);
	}
}

/*
 * A pointer into the loaded image for a kernel virtual address.
 *
 * The section headers already say where each address lives in the file, so
 * anything the guest can read at a link-time address can be read here too --
 * before it has run, and after it has stopped.
 */
static const unsigned char* co_elf_at_va(co_elf_data_t* pl, unsigned long long va)
{
	co_elf_off_t i, n = co_get_section_count(pl);

	for (i = 1; i < n; i++) {
		co_elf_shdr_t* sh = co_get_section_header(pl, i);

		if (!(sh->sh_flags & SHF_ALLOC) || sh->sh_type == SHT_NOBITS)
			continue;
		if (va < sh->sh_addr || va >= sh->sh_addr + sh->sh_size)
			continue;

		return pl->buffer + sh->sh_offset + (unsigned long)(va - sh->sh_addr);
	}

	return NULL;
}

/*
 * Say what a WARN_ON was about.
 *
 * WARN_ON compiles to ud2 plus an entry in __bug_table, and on real hardware
 * the kernel's own #UD handler is what turns that into a printed message. A
 * cooperative guest runs on the host's IDT, so that handler never runs: the
 * host steps over the instruction instead, and the text the kernel would have
 * printed is never produced by anyone.
 *
 * But the table is in the image, so the message can be recovered here. Every
 * field is a displacement relative to its own address, which is what makes the
 * table position independent; on x86-64 the entry carries a format string and,
 * with DEBUG_BUGVERBOSE, a file and line.
 */
static void co_report_bug_at(co_elf_data_t* pl, unsigned long long rip)
{
	co_elf_shdr_t* sec = co_get_section_by_name(pl, "__bug_table");
	const unsigned char* base;
	unsigned long long off;
	static const int sizes[] = { 16, 12, 8 };
	int si;

	if (!sec || !sec->sh_size) {
		co_terminal_print("      (no __bug_table in this image)\n");
		return;
	}

	base = pl->buffer + sec->sh_offset;

	/*
	 * The entry size depends on config, and the config is not in front of
	 * us. Take the first stride under which this table's own addresses all
	 * land inside itself -- a wrong stride produces nonsense immediately.
	 */
	for (si = 0; si < (int)(sizeof(sizes)/sizeof(sizes[0])); si++) {
		int stride = sizes[si];

		if (sec->sh_size % stride)
			continue;

		for (off = 0; off + stride <= sec->sh_size; off += stride) {
			const unsigned char* e = base + off;
			int disp = *(const int*)e;
			unsigned long long addr = sec->sh_addr + off + (long long)disp;

			if (addr != rip)
				continue;

			co_terminal_print("      %s", "");
			if (stride >= 16) {
				int fdisp = *(const int*)(e + 8);
				unsigned short line = *(const unsigned short*)(e + 12);
				unsigned long long fva = sec->sh_addr + off + 8 + (long long)fdisp;
				const unsigned char* fp = co_elf_at_va(pl, fva);

				if (fp)
					co_terminal_print("%s:%u", (const char*)fp, line);
				else
					co_terminal_print("file at 0x%llx line %u", fva, line);

				{
					int gdisp = *(const int*)(e + 4);
					unsigned long long gva = sec->sh_addr + off + 4
							       + (long long)gdisp;
					const unsigned char* gp = co_elf_at_va(pl, gva);

					if (gp && *gp)
						co_terminal_print("  \"%s\"", (const char*)gp);
				}
			}
			co_terminal_print("  flags 0x%x\n",
					  *(const unsigned short*)(e + stride - 2));
			return;
		}
	}

	co_terminal_print("      (no __bug_table entry for this address)\n");
}

co_rc_t co_elf_load_into_guest(const char* filename, int enter,
			       unsigned long max_switches, unsigned long batch,
			       const char* const* cobd, const char* init_path,
			       unsigned long mem_mb, int no_copic, int async_cobd)
{
	const char* cobd0 = cobd ? cobd[0] : NULL;
	co_elf_data_t* pl;
	co_manager_handle_t handle;
	co_manager_ioctl_kload_verify_t v = {0, };
	co_manager_ioctl_test_switch_t r = {0, };
	co_elf_off_t index, sections;
	unsigned long long lo = ~0ULL, hi = 0;
	unsigned long long text_va = 0;
	unsigned long size;
	unsigned long alloc_sections = 0, nobits_sections = 0;
	unsigned long long bytes = 0;
	char* buf;
	co_rc_t rc;
	bool_t installed = PFALSE;

	rc = co_os_file_load((char*)filename, &buf, &size, 0);
	if (!CO_OK(rc)) {
		co_terminal_print("cannot read %s\n", filename);
		return rc;
	}

	rc = co_elf_image_read(&pl, buf, size);
	if (!CO_OK(rc)) {
		co_terminal_print("not a usable ELF image for this build (rc %x)\n", (int)rc);
		co_os_file_free(buf);
		return rc;
	}

	/* The span the image needs, so the driver can bound every write to it. */
	sections = co_get_section_count(pl);
	for (index = 1; index < sections; index++) {
		co_elf_shdr_t* section = co_get_section_header(pl, index);

		if (!(section->sh_flags & SHF_ALLOC) || section->sh_size == 0)
			continue;

		if (section->sh_addr < lo)
			lo = section->sh_addr;
		if (section->sh_addr + section->sh_size > hi)
			hi = section->sh_addr + section->sh_size;
	}

	if (lo >= hi) {
		co_terminal_print("no allocatable sections\n");
		co_os_file_free(buf);
		return CO_RC(ERROR);
	}

	{
		co_elf_symbol_t* sym = co_get_symbol_by_name(pl, "_text");

		text_va = sym ? co_elf_get_symbol_value(sym) : lo;
	}

	co_terminal_print("%s\n", filename);
	co_terminal_print("  image spans 0x%016llx..0x%016llx  (%llu KB, %llu pages)\n",
			  lo, hi, (hi - lo) / 1024, (hi - lo + 4095) / 4096);

	rc = co_os_manager_is_installed(&installed);
	if (!CO_OK(rc) || !installed) {
		co_terminal_print("driver not installed\n");
		co_os_file_free(buf);
		return CO_RC(ERROR_ACCESSING_DRIVER);
	}

	handle = co_os_manager_open();
	if (!handle) {
		co_terminal_print("couldn't get driver handle\n");
		co_os_file_free(buf);
		return CO_RC(ERROR_MONITOR_NOT_LOADED);
	}

	rc = co_manager_kload_begin(handle, lo, hi);
	if (!CO_OK(rc)) {
		co_terminal_print("kload begin failed (rc %x)\n", (int)rc);
		goto out;
	}

	for (index = 1; index < sections; index++) {
		co_elf_shdr_t* section = co_get_section_header(pl, index);
		unsigned long long va;
		unsigned long long left;
		const unsigned char* p;
		int zero;

		if (!(section->sh_flags & SHF_ALLOC) || section->sh_size == 0)
			continue;

		zero = (section->sh_type == SHT_NOBITS);
		va   = section->sh_addr;
		left = section->sh_size;
		p    = zero ? NULL : co_get_at_offset(pl, section, 0);

		alloc_sections++;
		if (zero)
			nobits_sections++;
		bytes += left;

		while (left) {
			unsigned long part = (left > CO_KLOAD_CHUNK)
					   ? CO_KLOAD_CHUNK : (unsigned long)left;

			rc = co_manager_kload_chunk(handle, va, p, part, zero);
			if (!CO_OK(rc)) {
				co_terminal_print("  chunk at 0x%016llx (%lu bytes) failed (rc %x)\n",
						  va, part, (int)rc);
				goto out_end;
			}

			va   += part;
			left -= part;
			if (!zero)
				p += part;
		}
	}

	co_terminal_print("  %lu allocatable sections loaded (%lu of them nobits), %llu KB\n",
			  alloc_sections, nobits_sections, bytes / 1024);

	/*
	 * Read a stretch of .text back through the guest's page tables and compare
	 * against the same bytes in the file. This is what distinguishes a loader
	 * that wrote the image correctly from one that wrote it somewhere the guest
	 * cannot reach.
	 */
	{
		co_elf_shdr_t* text = co_get_section_by_name(pl, ".text");
		unsigned long check = 0x10000;
		unsigned long long want = 1469598103934665603ULL;
		const unsigned char* p;
		unsigned long i;

		if (text && text->sh_size >= check) {
			v.va   = text->sh_addr;
			v.size = check;

			rc = co_manager_kload_verify(handle, &v);
			if (!CO_OK(rc) || !CO_OK(v.rc)) {
				co_terminal_print("  verify failed (rc %x / %x)\n",
						  (int)rc, (int)v.rc);
				goto out_end;
			}

			p = co_get_at_offset(pl, text, 0);
			for (i = 0; i < check; i++) {
				want ^= p[i];
				want *= 1099511628211ULL;
			}

			co_terminal_print("  pages allocated    %lu\n", v.pages);
			co_terminal_print("  page-table pages   %lu\n", v.tables);
			co_terminal_print("  chunks written     %lu\n", v.chunks);
			co_terminal_print("\n");
			co_terminal_print("  .text first %lu bytes, read back through the guest tables:\n", check);
			co_terminal_print("    in the file  0x%016llx\n", want);
			co_terminal_print("    in the guest 0x%016llx   %s\n", v.checksum,
					  (want == v.checksum) ? "MATCH" : "DIFFERENT");
			if (want != v.checksum) {
				co_terminal_print("\n  the image is not where the guest would look for it\n");
				goto out_end;
			}
		}
	}

	if (enter == 2) {
		/*
		 * Run code the kernel compiled, rather than a stub of ours placed
		 * inside it. memset and strlen are leaves -- they touch their
		 * arguments and nothing else -- so they can run before a single
		 * line of kernel initialisation has.
		 */
		co_manager_ioctl_kcall_t k = {0, };
		bool_t have_console;
		co_elf_symbol_t* ms = co_get_symbol_by_name(pl, "memset");
		co_elf_symbol_t* sl = co_get_symbol_by_name(pl, "strlen");
		co_elf_symbol_t* sp = co_get_symbol_by_name(pl, "snprintf");
		co_elf_symbol_t* ep = co_get_symbol_by_name(pl, "early_printk");
		co_elf_symbol_t* ec = co_get_symbol_by_name(pl, "early_console");
		co_elf_symbol_t* cc = co_get_symbol_by_name(pl, "early_colinux_console");
		co_elf_symbol_t* cr = co_get_symbol_by_name(pl, "co_colinux_console_ring");

		if (!ms || !sl) {
			co_terminal_print("\n  memset or strlen not found in the image\n");
			goto out_end;
		}

		k.memset_va = co_elf_get_symbol_value(ms);
		k.strlen_va = co_elf_get_symbol_value(sl);
		k.snprintf_va = sp ? co_elf_get_symbol_value(sp) : 0;

		co_terminal_print("\n  calling code the kernel compiled:\n");
		co_terminal_print("    memset    0x%016llx\n", k.memset_va);
		co_terminal_print("    strlen    0x%016llx\n", k.strlen_va);
		if (k.snprintf_va)
			co_terminal_print("    snprintf  0x%016llx\n", k.snprintf_va);

		have_console = (ep && ec && cc && cr) ? PTRUE : PFALSE;
		if (have_console) {
			k.early_printk_va    = co_elf_get_symbol_value(ep);
			k.early_console_va   = co_elf_get_symbol_value(ec);
			k.colinux_console_va = co_elf_get_symbol_value(cc);
			k.ring_symbol_va     = co_elf_get_symbol_value(cr);
			co_terminal_print("\n  the patched early console:\n");
			co_terminal_print("    early_printk             0x%016llx\n", k.early_printk_va);
			co_terminal_print("    early_console (global)   0x%016llx\n", k.early_console_va);
			co_terminal_print("    early_colinux_console    0x%016llx\n", k.colinux_console_va);
			co_terminal_print("    co_colinux_console_ring  0x%016llx\n", k.ring_symbol_va);
		} else {
			co_terminal_print("\n  (image has no cooperative console -- unpatched kernel)\n");
		}
		co_terminal_print("\n");

		rc = co_manager_kcall(handle, &k);
		if (!CO_OK(rc) || !CO_OK(k.rc)) {
			co_terminal_print("  kcall failed (rc %x / %x)\n", (int)rc, (int)k.rc);
			goto out_end;
		}
		if (!k.supported) {
			co_terminal_print("  not implemented on this architecture\n");
			goto out_end;
		}
		if (k.faulted) {
			co_terminal_print("  FAULT: vector %llu at rip 0x%016llx, cr2 0x%016llx\n",
					  k.vector, k.fault_rip, k.cr2);
			goto out_end;
		}

		co_terminal_print("  memset(0x%016llx, 0x5a, 4096)\n", k.scratch_va);
		co_terminal_print("    returned      0x%016llx   %s\n", k.memset_ret,
				  (k.memset_ret == k.scratch_va) ? "(the destination, as it should)" : "WRONG");
		co_terminal_print("    page contents %s\n",
				  k.pattern_ok ? "all 0x5a -- it really wrote them"
					       : "NOT all 0x5a");
		if (!k.pattern_ok)
			co_terminal_print("      first wrong byte at offset %d: 0x%02x\n",
					  k.first_bad, k.first_bad_byte);

		co_terminal_print("\n  strlen(\"%s\")\n", "hello from a cooperative guest");
		co_terminal_print("    returned      %llu   (expected %llu)   %s\n",
				  k.strlen_ret, k.strlen_expected,
				  (k.strlen_ret == k.strlen_expected) ? "MATCH" : "WRONG");
		if (k.snprintf_va) {
			co_terminal_print("\n  snprintf(buf, 256, \"colinux: %%s, %%d-bit, ok\", \"x86-64\", 64)\n");
			co_terminal_print("    returned      %llu   (expected %llu)   %s\n",
					  k.snprintf_ret, k.snprintf_expected,
					  (k.snprintf_ret == k.snprintf_expected) ? "MATCH" : "WRONG");
			co_terminal_print("\n");
			co_terminal_print("    the kernel formatted this, and we read it out of guest memory:\n");
			co_terminal_print("      >> %s\n", k.text);
			co_terminal_print("    %s\n", k.text_ok ? "exactly as expected"
							     : "NOT what was expected");
		}

		/*
		 * Gate on the local flag, not on k.early_printk_va: that field
		 * travels inwards and the driver clears the struct before filling
		 * in results, so it reads back as zero.
		 */
		if (have_console) {
			co_terminal_print("\n  early_printk(\"colinux: early console alive, %%s, %%d-bit\\n\", \"x86-64\", 64)\n");
			co_terminal_print("    ring at       0x%016llx  (in the passage page)\n", k.console_ring_va);
			co_terminal_print("    bytes written %llu of %llu capacity%s\n",
					  k.console_written, k.console_capacity,
					  (k.console_written > k.console_capacity) ? "  TRUNCATED" : "");
			co_terminal_print("\n");
			co_terminal_print("    what the kernel printed:\n");
			co_terminal_print("      >> %s", k.console_text);
			if (!k.console_ok)
				co_terminal_print("      (nothing -- the console did not write)\n");
		}

		co_terminal_print("\n");

		if (k.succeeded)
			co_terminal_print("  RAN LINUX'S OWN COMPILED CODE. Three functions out of the\n"
					  "  loaded image executed in the guest address space: one verified\n"
					  "  by the bytes it wrote, one by the value it returned, and the\n"
					  "  kernel's whole formatting engine by the text it produced.\n");
		else
			co_terminal_print("  the calls returned but did not do what they should have\n");

		goto out_end;
	}

	if (enter == 3) {
		co_manager_ioctl_kboot_t b = {0, };
		co_manager_ioctl_kram_t  m = {0, };
		struct co_klog_ctx	 klog_ctx = {0, };
		void*			 klog_thread = NULL;
		unsigned long long	 klog_since = 0;
		unsigned char bp[4096];
		/*
		 * Sized against COMMAND_LINE_SIZE (2048 on x86), not against
		 * what the string happens to be today.
		 *
		 * At 256 this overflowed the moment root= was added: the
		 * options are 226 characters and the root arguments another
		 * 51, so strcat ran off the end of a stack buffer and what
		 * reached the guest stopped at "rootfstype=ex". The kernel
		 * then asked for a filesystem called "ex", get_fs_type()
		 * returned -ENODEV, and the boot panicked with "Cannot open
		 * root device ... error -19" -- which reads as a broken block
		 * device and is nothing of the kind.
		 */
		char cmdline[1024];
		co_elf_symbol_t* s_bp;
		co_elf_symbol_t* s_cl;
		co_elf_symbol_t* s_text;
		co_elf_symbol_t* s_end;
		unsigned long long ram = ((unsigned long long)
			(mem_mb ? mem_mb : CO_GUEST_RAM_DEFAULT_MB)) << 20;
		static const char* want[] = { "co_arch_start_kernel", "initial_code",
					      "start_kernel", "early_console",
					      "early_colinux_console",
					      "co_colinux_console_ring",
					      "co_colinux_guest",
					      /*
					       * The cooperative timer: where to
					       * vector a running guest, and the
					       * flag that says whether it may be
					       * vectored at all. Both are needed
					       * for the host to be able to
					       * interrupt a guest that is not
					       * cooperating -- see the injection
					       * in arch/x86_64/switch.c.
					       */
					      "asm_sysvec_co_timer",
					      "co_colinux_virtual_if", NULL };
		unsigned long long addr[9];
		int i;

		for (i = 0; want[i]; i++) {
			co_elf_symbol_t* sym = co_get_symbol_by_name(pl, want[i]);

			if (!sym) {
				co_terminal_print("\n  %s not found -- is the kernel patched?\n",
						  want[i]);
				goto out_end;
			}
			addr[i] = co_elf_get_symbol_value(sym);
		}

		/*
		 * Physical memory, and a linear map of it.
		 *
		 * setup_arch reads an e820 map, hands the ranges to memblock, and
		 * from then on turns physical addresses back into virtual ones with
		 * __va(). Without RAM behind those addresses and a map at
		 * PAGE_OFFSET, the first allocation the kernel dereferences faults
		 * -- and by then it has replaced our stubs, so the fault is
		 * unreportable. This is what was missing when it took the host down.
		 */
		s_text = co_get_symbol_by_name(pl, "_text");
		s_end  = co_get_symbol_by_name(pl, "_end");
		if (!s_text || !s_end) {
			co_terminal_print("\n  _text or _end missing\n");
			goto out_end;
		}

		m.ram_bytes = ram;
		m.text_va   = co_elf_get_symbol_value(s_text);
		m.end_va    = co_elf_get_symbol_value(s_end);

		co_terminal_print("\n  giving the guest RAM and a linear map at\n");
		co_terminal_print("  PAGE_OFFSET, with the image visible at both its link\n");
		co_terminal_print("  addresses and its physical ones\n");

		rc = co_manager_kram(handle, &m);
		if (!CO_OK(rc) || !CO_OK(m.rc)) {
			co_terminal_print("  building guest RAM failed (rc %x / %x)\n",
					  (int)rc, (int)m.rc);
			goto out_end;
		}
		co_terminal_print("    %lu pages mapped, %lu page-table pages total\n",
				  m.ram_pages, m.tables);
		co_terminal_print("    %d blocks, %llu MB usable of %llu MB asked\n",
				  m.range_count, m.total_usable >> 20, ram >> 20);

		/*
		 * The root device, attached before the guest runs so that the
		 * driver's probe finds it. Fatal if it was asked for and could
		 * not be opened: booting on anyway would reach prepare_namespace
		 * and panic about the root filesystem, which says nothing about
		 * the file that is actually missing or locked.
		 */
		for (i = 0; cobd && i < CO_COBD_MAX_UNITS; i++) {
			unsigned long long dsize = 0;

			if (!cobd[i])
				continue;

			rc = co_manager_cobd(handle, i, cobd[i], &dsize);
			if (!CO_OK(rc)) {
				co_terminal_print("\n  cannot attach cobd%d to '%s' (rc %x)\n",
						  i, cobd[i], (int)rc);
				co_terminal_print("  the driver opens this path itself, so it is an NT\n");
				co_terminal_print("  object path, and it must not be a volume Windows has mounted\n");
				goto out_end;
			}

			co_terminal_print("    cobd%d -> %s\n", i, cobd[i]);
			if (i == 0)
				co_terminal_print("          %llu MB, root=/dev/cobd0\n", dsize >> 20);
			else
				co_terminal_print("          %llu MB, /dev/cobd%d\n", dsize >> 20, i);
		}

		/*
		 * boot_params, written straight into the guest at its symbol.
		 *
		 * x86_64_start_kernel would normally build this with copy_bootdata,
		 * and we skip that function because it reloads CR3. Skipping it
		 * without doing its job is what left the kernel believing it had no
		 * memory at all.
		 */
		s_bp = co_get_symbol_by_name(pl, "boot_params");
		s_cl = co_get_symbol_by_name(pl, "boot_command_line");
		if (!s_bp || !s_cl) {
			co_terminal_print("\n  boot_params or boot_command_line missing\n");
			goto out_end;
		}

		/*
		 * The e820 describes where the memory really is.
		 *
		 * Guest physical addresses are host physical addresses, and the
		 * memory comes in several contiguous blocks -- one usable entry
		 * per block, at its true address. There is no low memory and no
		 * hole to invent, because there is no emulated machine
		 * underneath; a fragmented map is nothing unusual to Linux,
		 * real machines have holes too.
		 *
		 * The page-table region at the top of block 0 goes in as type
		 * 3, E820_TYPE_ACPI, and the type is load bearing rather than
		 * decorative. phys_pmd_init() walks the direct map two
		 * megabytes at a time and, for any span past what it is
		 * currently mapping, does this:
		 *
		 *     if (!e820__mapped_any(.., E820_TYPE_RAM) &&
		 *         !e820__mapped_any(.., E820_TYPE_ACPI))
		 *             set_pmd_init(pmd, __pmd(0), init);
		 *
		 * -- it *zeroes* the entry. Described as type 2 the region is
		 * neither RAM nor ACPI, so the kernel deleted the host's
		 * mapping for the very pages its own page tables live in: two
		 * warnings from set_pmd_safe, one per 2 MB, and then a fault
		 * the next time anything called __va() on a table page.
		 *
		 * ACPI memory is spared by that test because it is exactly this
		 * kind of region -- must stay mapped, must never be allocated
		 * over -- and memblock still does not hand it out, since only
		 * RAM becomes available memory. Which is the whole requirement.
		 */
		memset(bp, 0, sizeof(bp));
		{
			int n = 0;
			/*
			 * E820_MAX_ENTRIES_ZEROPAGE. The kernel reads exactly
			 * this many out of boot_params and the count is a
			 * single byte at 0x1e8, so writing past it would both
			 * overrun this buffer and wrap the count -- a guest
			 * told it has four ranges when it was given 260.
			 * Refuse instead: the block cap is 40, so this cannot
			 * fire today, which is precisely when a bound is worth
			 * writing rather than after it has.
			 */
			const int max_entries = 128;

			for (i = 0; i < m.range_count; i++) {
				/*
				 * What this range actually costs: one entry, and
				 * a second only if it carries the reserved
				 * page-table region, which is block 0 alone.
				 *
				 * Asking for two every time was wrong by exactly
				 * one, and it rejected the case the block cap was
				 * chosen to permit: CO_KLOAD_MAX_BLOCKS is 127
				 * because 1 block at two entries plus 126 at one
				 * is 128 on the nose. A 127-block guest -- which
				 * is what a fragmented host hands back for a 1 GB
				 * target -- refused to boot at all.
				 */
				int need = m.range[i].reserved ? 2 : 1;

				if (n + need > max_entries) {
					co_terminal_print("\n  e820 needs more than %d entries for %d"
							  " ranges -- refusing to describe a guest\n"
							  "  differently from the one that was built\n",
							  max_entries, m.range_count);
					goto out_end;
				}
				co_e820_entry(bp + 0x2d0 + n * 20,
					      m.range[i].pa, m.range[i].usable, 1);
				n++;
				if (m.range[i].reserved) {
					co_e820_entry(bp + 0x2d0 + n * 20,
						      m.range[i].pa + m.range[i].usable,
						      m.range[i].reserved, 3);
					n++;
				}
			}
			bp[0x1e8] = (unsigned char)n;		/* e820_entries */
		}

		rc = co_manager_kload_chunk(handle, co_elf_get_symbol_value(s_bp),
					    bp, sizeof(bp), 0);
		if (!CO_OK(rc)) {
			co_terminal_print("  writing boot_params failed (rc %x)\n", (int)rc);
			goto out_end;
		}

		/*
		 * Keep the guest's hands off the interrupt hardware.
		 *
		 * There is one physical machine here and Windows is using it.
		 * The guest has already read the host's real firmware -- its
		 * DMI strings, its MP-table, its ACPI tables -- because guest
		 * physical is host physical and early_ioremap() of 0xf0000
		 * finds the M92p's actual BIOS. Reading is survivable. What
		 * comes next is not: apic_bsp_setup() calls setup_local_APIC()
		 * and setup_IO_APIC(), which write the enable bit, the task
		 * priority, the logical destination and the whole IOAPIC
		 * redirection table of the chips Windows takes its timer,
		 * keyboard and disk interrupts through.
		 *
		 * That this can happen is not a guess. The guest has already
		 * driven real hardware once: "Fast TSC calibration using PIT"
		 * is it programming the 8254 through ports 0x40-0x43 and
		 * reading back the M92p's true 3193 MHz. The PIT is vestigial
		 * under XP x64, so nothing came of it. The APIC is not.
		 *
		 *   nolapic         apic_is_disabled, so apic_intr_mode is
		 *                   APIC_PIC and apic_intr_mode_init() returns
		 *                   before any of those four calls.
		 *   acpi=off        no table parsing and no interpreter. The
		 *                   wider hazard of the two: acpi_enable()
		 *                   writes SMI_CMD to take ACPI ownership from
		 *                   firmware that has already given it to
		 *                   Windows, and AML, once running, can write
		 *                   any port or physical address it likes.
		 *   noapic, nohpet,
		 *   no_timer_check  explicit rather than implied by the above.
		 *
		 * None of it is a loss. This guest runs with interrupts
		 * disabled and never takes one -- the host forwards its own in
		 * host context -- so an interrupt controller of its own is
		 * machinery it cannot use, pointed at hardware it must not
		 * touch. The i386 port gives the guest a virtual controller
		 * instead, and that is where this ends up.
		 */
		/*
		 * noxsave, so the two sides agree on what "extended state" is.
		 *
		 * fpu__init_cpu_xstate() sets CR4.OSXSAVE and then executes
		 * xsetbv, which raises #UD if that bit did not take -- and it
		 * does not take here. cr4_set_bits() computes from
		 * cpu_tlbstate.cr4, a shadow primed by cr4_init_shadow() from
		 * x86_64_start_kernel(), which a cooperative guest never runs.
		 * It reads back as all ones, and the host refuses to write a
		 * CR4 with bits above 31 set, correctly.
		 *
		 * The shadow is worth priming eventually; every other
		 * cr4_set_bits() caller meets the same wall. But it is the
		 * wrong fix for this one. The passage page saves 512 bytes of
		 * FXSAVE state per crossing, and that was measured as the whole
		 * of it because this host runs with OSXSAVE clear -- CR4 0x6f8.
		 * A guest that turns XSAVE on invalidates that measurement and
		 * needs XSAVE-sized state handling on both sides of every
		 * switch. Declining the feature keeps one save format across
		 * the crossing, which is the same answer the host already gave.
		 */
		/*
		 * disable_mtrr_trim, so the guest keeps the memory it is given.
		 *
		 * mtrr_trim_uncached_memory() discards RAM the host's MTRRs do
		 * not describe as write-back. It took a block at 0x104200000
		 * away after the guest had already counted it -- last_pfn went
		 * 0x106200 to 0x2bc00 in the space of one log line.
		 *
		 * The first answer to that was to stop allocating above 4 GB,
		 * which was the wrong layer: it left the trimming in place and
		 * halved the range the allocator searches, so on a host that
		 * had been up a day the image block could not be found at all.
		 * This turns off the trimming instead, which is the thing that
		 * was actually wrong. The host's MTRRs describe the host's
		 * memory correctly; it is the guest's idea of which RAM is its
		 * own that they say nothing useful about.
		 */
		/*
		 * No idle= override: select_idle_routine sees co_colinux_guest
		 * and installs the cooperative idle, which yields to the host
		 * through the passage page instead of halting or mwaiting. The
		 * ud2;ud2 traps in native_halt/native_safe_halt remain as a
		 * backstop for any halt reached some other way.
		 *
		 * This guest must also never discover or initialize the physical
		 * machine underneath Windows.  The first native-speed boot found the
		 * host's PCI bus and ran ata_piix against its live SATA controller;
		 * the ioctl returned cleanly, then Windows froze because its storage
		 * controller had been reprogrammed.  The kernel has hard PIO/MMIO and
		 * PCI guards as the security boundary.  These options also select the
		 * no-hardware paths early enough to avoid pointless native probes and
		 * keep per-CPU PAT, MCE, microcode and watchdog state owned by Windows.
		 */
		/*
		 * lpj= presets the delay-loop calibration. calibrate_delay()
		 * otherwise spins waiting for jiffies to advance, and virtual
		 * time is only delivered at idle boundaries -- which the boot
		 * has not reached yet, so the spin never ends. The value is
		 * loops per jiffy at HZ=1000 for the M92p's ~3.2 GHz core;
		 * udelay lengths scale with it, and nothing here drives
		 * hardware that cares about a few percent.
		 */
		memset(cmdline, 0, sizeof(cmdline));
		strcpy(cmdline, "earlyprintk=colinux,keep console=earlycolinux"
				/*
				 * The interactive console, last so it is the
				 * one /dev/console resolves to: the kernel
				 * makes the final console= on the line the
				 * one init inherits. earlycolinux stays for
				 * the boot log, which is drained after the
				 * run and is the only record if the guest
				 * dies before hvc0 is up.
				 */
				" console=hvc0"
				" acpi=off noapic nolapic nohpet no_timer_check"
				" noxsave noxsaveopt noxsaves disable_mtrr_trim"
				" pci=off nopat io_delay=none mce=off dis_ucode_ldr"
				" nowatchdog 8250.nr_uarts=0 lpj=1600000"
				/*
				 * A clock. The guest has had none, and it cost
				 * more than a missing feature: it made every
				 * measurement this project has taken of a boot
				 * meaningless.
				 *
				 * What the guest says about itself, read live:
				 *
				 *   tsc: Fast TSC calibration failed
				 *   tsc: Unable to calibrate against PIT
				 *   tsc: No reference (HPET/PMTIMER) available
				 *   tsc: Marking TSC unstable due to could not
				 *        calculate TSC khz
				 *   clocksource: Switched to refined-jiffies
				 *
				 * Three calibration routes and all three are shut
				 * by the options above this line, deliberately:
				 * the PIT belongs to Windows, nohpet removes the
				 * HPET, acpi=off removes the PM timer. So it falls
				 * back to jiffies -- and jiffies only advance when
				 * the host delivers ticks, at the idle boundary
				 * and the exit-to-user drain, neither of which a
				 * booting kernel reaches.
				 *
				 * The consequence is that sched_clock stands still
				 * for the whole kernel phase. Every printk
				 * timestamp in every boot log this project has is
				 * [0.000000] up to "Run /sbin/init", then jumps to
				 * 1.53 the moment userspace exists and the drain
				 * starts running. A kernel phase that takes
				 * minutes of wall clock is recorded as four
				 * milliseconds, which is why systemd-analyze has
				 * always reported a 9 ms kernel and why nobody
				 * ever saw that booting is slow.
				 *
				 * tsc_early_khz hands over the frequency instead
				 * of asking the guest to measure it, exactly as
				 * lpj= above hands over the delay loop instead of
				 * calibrating it. The number is not invented: the
				 * host reads this core at 3193 MHz through the PIT
				 * during its own early boot, and the guest itself
				 * measured 3193 MHz the one time it was allowed to
				 * drive the 8254.
				 *
				 * tsc=reliable stops the watchdog marking it
				 * unstable again. There is no second clocksource
				 * here to check the TSC against, so the watchdog
				 * can only ever conclude that the one clock is
				 * wrong -- which is what it did.
				 */
				" tsc_early_khz=3193000 tsc=reliable"
				/*
				 * No PCID. The guest owns CR3 now, and with
				 * CR4.PCIDE set the kernel puts an address
				 * space identifier in the low bits of every
				 * value it loads and stops issuing full
				 * flushes. The host measured CR4 as 0x6f8 --
				 * PCIDE clear -- so the world switch has never
				 * carried a tagged CR3 across, and a stale
				 * entry for the wrong address space is the
				 * kind of fault that lands nowhere near its
				 * cause. One variable fewer while ring 3 is
				 * the thing under test.
				 */
				" nopcid"
				/*
				 * No uncore PMU. The guest's boot CPU is
				 * whichever physical core the monitor thread
				 * was pinned on, so its real APIC ID may not
				 * be the first one in the host firmware's
				 * MP-table -- the kernel then builds an
				 * inconsistent topology ("Boot CPU APIC ID
				 * not the first enumerated") and intel_uncore
				 * indexes its per-package array with garbage:
				 * an Oops in allocate_boxes and a dead PID 1,
				 * on exactly the boots that landed on the
				 * wrong core. A cooperative guest cannot use
				 * the uncore PMU anyway -- Windows owns the
				 * hardware counters.
				 */
				" initcall_blacklist=intel_uncore_init");

		/*
		 * A root filesystem, if the host attached one. Without it the
		 * kernel reaches prepare_namespace with nothing to mount and
		 * panics -- which is the correct end for a kernel with no
		 * disk, and is exactly where the boot stopped before cobd
		 * existed.
		 */
		if (cobd0) {
			strcat(cmdline, " root=/dev/cobd0 rootfstype=ext4 rw init=");
			/*
			 * Overridable, because the first thing worth knowing
			 * about an unfamiliar root filesystem is whether it
			 * can run anything at all. Booting straight into an
			 * init system conflates "the userspace works" with
			 * "the init system works", and when it stops there is
			 * no way to tell which failed. --init /bin/sh answers
			 * the first question on its own.
			 */
			strcat(cmdline, init_path ? init_path : "/sbin/init");
		}

		/*
		 * Refuse rather than truncate. A command line that is silently
		 * cut short does not fail where it was built, it fails deep in
		 * the guest as a wrong-looking kernel error, which is exactly
		 * how the 256-byte buffer cost a boot.
		 */
		if (strlen(cmdline) >= sizeof(cmdline) - 1) {
			co_terminal_print("\n  the kernel command line does not fit in %d bytes\n",
					  (int)sizeof(cmdline));
			goto out_end;
		}
		co_terminal_print("    cmdline is %d bytes\n", (int)strlen(cmdline));
		rc = co_manager_kload_chunk(handle, co_elf_get_symbol_value(s_cl),
					    cmdline, sizeof(cmdline), 0);
		if (!CO_OK(rc)) {
			co_terminal_print("  writing boot_command_line failed (rc %x)\n", (int)rc);
			goto out_end;
		}

		/*
		 * phys_base, which is how __pa() of a kernel address is computed:
		 *
		 *     __pa(x) = x - __START_KERNEL_map + phys_base
		 *
		 * The driver chose where the image landed -- at the base of its
		 * block, with the link address's 16 MB offset absorbed into
		 * phys_base rather than allocated -- so phys_base is whatever
		 * it reports, not something derived here. Left at zero the
		 * kernel would compute physical addresses for its own text
		 * that are nowhere near where it actually is.
		 */
		{
			co_elf_symbol_t* s_pb = co_get_symbol_by_name(pl, "phys_base");

			if (!s_pb) {
				co_terminal_print("\n  phys_base not found\n");
				goto out_end;
			}

			rc = co_manager_kload_chunk(handle, co_elf_get_symbol_value(s_pb),
						    (unsigned char*)&m.phys_base,
						    sizeof(m.phys_base), 0);
			if (!CO_OK(rc)) {
				co_terminal_print("  writing phys_base failed (rc %x)\n", (int)rc);
				goto out_end;
			}
			co_terminal_print("    phys_base = 0x%llx\n", m.phys_base);
		}

		for (i = 0; i < m.range_count; i++) {
			co_terminal_print("    e820: 0x%llx-0x%llx usable (%llu MB)%s\n",
					  m.range[i].pa,
					  m.range[i].pa + m.range[i].usable,
					  m.range[i].usable >> 20,
					  m.range[i].reserved ? ", then the tables" : "");
			if (m.range[i].reserved)
				co_terminal_print("          0x%llx-0x%llx ACPI, so the kernel"
						  " keeps it mapped (%llu MB)\n",
						  m.range[i].pa + m.range[i].usable,
						  m.range[i].pa + m.range[i].usable
						  + m.range[i].reserved,
						  m.range[i].reserved >> 20);
		}
		co_terminal_print("    cmdline: %s\n", cmdline);

		b.entry_va           = addr[0];
		b.initial_code_va    = addr[1];
		b.start_kernel_va    = addr[2];
		b.early_console_va   = addr[3];
		b.colinux_console_va = addr[4];
		b.ring_symbol_va     = addr[5];
		b.guest_flag_va      = addr[6];
		/*
		 * Withholding the entry address is how --no-copic works: the
		 * host refuses to inject when it has nowhere to inject to, so
		 * one flag turns the whole mechanism off with no second code
		 * path to keep correct.
		 */
		b.tick_entry_va      = no_copic ? 0 : addr[7];
		b.virtual_if_va      = addr[8];
		if (no_copic)
			co_terminal_print("    cooperative timer disabled (--no-copic):"
					  " a running guest will not be interrupted\n");
		/*
		 * Free-running. The guest runs at native speed and comes back on
		 * its own cooperative yields, warnings and recoverable faults --
		 * single-stepping was the bring-up scaffold, and what it bought
		 * (a bound on a guest that never yields) the cooperative idle
		 * now provides. Real IF stays clear the whole time (the guest's
		 * interrupt flag is virtual), so the host is deaf only from
		 * entry to the first crossing -- a boot's worth at native speed,
		 * not 90 seconds of stepping. --batch N restores stepped mode.
		 */
		b.step         = batch ? 1 : 0;
		b.max_switches = max_switches ? max_switches : 200000;
		/*
		 * Instructions per crossing, chosen as a length of time rather
		 * than a round number.
		 *
		 * The guest steps this many before handing the processor back,
		 * which is what makes stepping affordable: one world switch per
		 * instruction measured 3.6us and nearly all of it was the
		 * switch, against about 500ns for a trap handled in the guest.
		 *
		 * But the guest runs with interrupts disabled, so the batch is
		 * also how long the host is deaf on this processor -- and the
		 * thread is pinned, so it is the same processor every time. At
		 * 4096 that is two milliseconds per crossing. A hundred
		 * crossings of it is survivable and four thousand is not: the
		 * machine stopped responding and needed the power button, with
		 * the run's own log showing it never completed a single
		 * crossing.
		 *
		 * 256 is about 130us, which is the same order as the latency a
		 * disk interrupt already imposes, and it costs almost nothing:
		 * the crossing is 3.6us against 128us of stepping, so 97% of
		 * the batching win is kept.
		 */
		b.batch        = batch;

		/*
		 * The kernel's own page tables, init_top_pgt first. The host
		 * relocates them to where the image really is and switches the
		 * guest into them: the kernel walks and edits these directly,
		 * and a space the host invented is not one it can work in.
		 */
		{
			static const char* const tnames[] = {
				"init_top_pgt", "level3_kernel_pgt",
				"level2_kernel_pgt", "level2_fixmap_pgt",
				"level1_fixmap_pgt", NULL
			};
			int t;

			b.kernel_table_count = 0;
			for (t = 0; tnames[t]; t++) {
				co_elf_symbol_t* sym = co_get_symbol_by_name(pl, tnames[t]);

				if (!sym) {
					co_terminal_print("\n  %s not found\n", tnames[t]);
					goto out_end;
				}
				b.kernel_tables[b.kernel_table_count++] =
					co_elf_get_symbol_value(sym);
			}
		}

		/*
		 * The guest's exception table.
		 *
		 * Linux takes faults on purpose and recovers from them through
		 * this table -- segment loads, rdmsr_safe, every user copy. Its
		 * own handlers are not installed in a cooperative guest, so the
		 * host reads the entries and does what they say.
		 */
		{
			co_elf_symbol_t* s_exs = co_get_symbol_by_name(pl, "__start___ex_table");
			co_elf_symbol_t* s_exe = co_get_symbol_by_name(pl, "__stop___ex_table");

			if (s_exs && s_exe) {
				b.ex_table_start = co_elf_get_symbol_value(s_exs);
				b.ex_table_stop  = co_elf_get_symbol_value(s_exe);
			} else {
				co_terminal_print("\n  __ex_table symbols missing -- faults the\n"
						  "  kernel means to recover from will stop the run\n");
			}
		}

		/*
		 * Where the guest keeps its pointer to the passage page. The
		 * driver writes the page's address there before entry, and the
		 * guest's cooperative idle calls the world switch through it.
		 */
		{
			co_elf_symbol_t* s_pp = co_get_symbol_by_name(pl, "co_colinux_passage_page");

			if (s_pp) {
				b.passage_symbol_va = co_elf_get_symbol_value(s_pp);
			} else {
				co_terminal_print("\n  co_colinux_passage_page missing -- the guest\n"
						  "  cannot yield cooperatively and idle will spin\n");
			}
		}

		/*
		 * And where it keeps the interactive console's rings, so a
		 * second process can serve a terminal against them while this
		 * one is inside the boot ioctl. Absent is not fatal: a guest
		 * without it boots exactly as before, with the one-way early
		 * console and no way in.
		 */
		{
			co_elf_symbol_t* s_cio = co_get_symbol_by_name(pl, "co_colinux_console_io");
			co_elf_symbol_t* s_nio = co_get_symbol_by_name(pl, "co_colinux_net_io");
			co_elf_symbol_t* s_bio = co_get_symbol_by_name(pl, "co_colinux_cobd_io");

			if (s_cio) {
				b.console_io_va = co_elf_get_symbol_value(s_cio);
				co_terminal_print("    console rings at 0x%016llx"
						  "  (--console PORT to attach)\n",
						  b.console_io_va);
			}
			if (s_nio) {
				b.net_io_va = co_elf_get_symbol_value(s_nio);
				co_terminal_print("    net rings at     0x%016llx"
						  "  (--net-dump to watch)\n",
						  b.net_io_va);
			}
			/*
			 * The async block completion ring. If the guest kernel
			 * predates it the symbol is absent, so cobd_io_va stays 0
			 * and the host silently keeps the inline sync path -- an
			 * old vmlinux still boots.
			 */
			if (s_bio && async_cobd) {
				b.cobd_io_va = co_elf_get_symbol_value(s_bio);
				b.async_cobd = 1;
				co_terminal_print("    cobd ring at     0x%016llx"
						  "  (async block I/O)\n",
						  b.cobd_io_va);
			} else {
				b.async_cobd = 0;
				co_terminal_print("    block I/O is synchronous%s\n",
						  s_bio ? " (--sync-cobd)"
							: " (guest has no cobd ring)");
			}

			/*
			 * The GPU transport, and the one thing only this code
			 * can do: tell the guest the device exists.
			 *
			 * The guest's transport driver runs at device_initcall
			 * and looks for a magic word. Nothing else is in a
			 * position to write it -- the GPU daemon has not
			 * started, and could not reach guest memory before the
			 * image is loaded anyway -- so the boot daemon fills in
			 * the header here, between loading the image and
			 * starting the guest. This mirrors the cobd enabled-word
			 * precedent exactly.
			 *
			 * An absent symbol means a kernel without the transport,
			 * which is normal and silent: vgpu_io_va stays zero, the
			 * initcall finds no magic, and the guest boots with no
			 * GPU.
			 */
			{
				co_elf_symbol_t* s_gio =
					co_get_symbol_by_name(pl, "co_colinux_vgpu_io");

				if (s_gio) {
					struct {
						unsigned int	   magic;
						unsigned int	   abi_version;
						unsigned long long host_features;
						unsigned long long guest_features;
						unsigned int	   status;
						unsigned int	   enabled;
						unsigned int	   num_capsets;
						unsigned int	   num_scanouts;
					} hdr;

					memset(&hdr, 0, sizeof(hdr));

					b.vgpu_io_va = co_elf_get_symbol_value(s_gio);

					hdr.magic	  = 0x55504756;	/* 'VGPU' */
					hdr.abi_version	  = 1;
					/*
					 * Two capsets: VIRGL and VIRGL2. Mesa's
					 * virgl driver looks for VIRGL2 and
					 * refuses the device without it. The
					 * guest asks
					 * for its contents at probe and Mesa
					 * refuses to use the device without
					 * them, so a zero here is a device
									 * nothing can render on.
					 * The daemon answers the query from
					 * virgl_renderer_get_cap_set, so what
					 * is advertised is what the host GL
					 * can actually do.
					 */
					hdr.num_capsets	  = 2;
					/*
					 * VERSION_1 (bit 32), VIRGL (bit 0) and
					 * CONTEXT_INIT (bit 4).
					 *
					 * CONTEXT_INIT is how a modern Mesa
					 * creates a 3D context and names the
					 * capset it wants; without it the
					 * driver refuses the ioctl outright --
					 * its only gate is has_context_init --
					 * so leaving it out of this word means
					 * no guest client can ever open a
					 * context, which is not a limitation
					 * anyone would guess from the symptom.
					 */
hdr.host_features = (1ULL << 32) |	/* VERSION_1        */
							    (1ULL << 0)  |	/* VIRGL            */
							    (1ULL << 2)  |	/* RESOURCE_UUID    */
							    (1ULL << 3)  |	/* RESOURCE_BLOB    */
							    (1ULL << 4);	/* CONTEXT_INIT     */

					/*
					 * The same path every other byte of the
					 * image took: the pages are already
					 * allocated and mapped, and the guest is
					 * not running yet.
					 */
					if (CO_OK(co_manager_kload_chunk(handle, b.vgpu_io_va,
									 &hdr, sizeof(hdr), 0))) {
						co_terminal_print("    vgpu transport at 0x%016llx"
								  "  (virtio-gpu, render-only)\n",
								  b.vgpu_io_va);
					} else {
						co_terminal_print("    vgpu transport present but"
								  " its header could not be written\n");
						b.vgpu_io_va = 0;
					}
				}
			}
		}

		co_terminal_print("\n  booting:\n");
		if (b.max_switches == ~0UL)
			co_terminal_print("    no switch limit and no deadline"
					  " -- stop.bat ends the run\n");
		else
			co_terminal_print("    stopping after %d world switches%s\n",
					  b.max_switches,
					  max_switches ? "  (--max-switches)" : "");
		if (b.step)
			co_terminal_print("    stepped, %d instructions per crossing  (--batch)\n",
					  b.batch);
		else
			co_terminal_print("    free-running: native speed, cooperative yields\n");
		for (i = 0; want[i]; i++)
			co_terminal_print("    %-24s 0x%016llx\n", want[i], addr[i]);

		co_terminal_print("\n  entering co_arch_start_kernel with real IF held clear,\n");
		co_terminal_print("  initial_code pointed at start_kernel, the early console\n");
		co_terminal_print("  wired, and cooperative safe points returning to Windows\n\n");
		co_terminal_print("  ---- live kernel log (streamed as the guest runs) ----\n");

		/*
		 * Stream the kernel log while the run is in progress. The thread
		 * reads on its own handle and is joined below before KLOAD_END,
		 * so it cannot outlive the guest's mapped memory. If the thread
		 * cannot start, the run still proceeds and the post-run dump
		 * covers it -- the stream is an addition, not a dependency.
		 */
		klog_ctx.handle = co_os_manager_open_quite();
		klog_ctx.pl     = pl;
		klog_ctx.since  = &klog_since;
		klog_ctx.stop   = 0;
		if (klog_ctx.handle)
			klog_thread = co_os_thread_start(co_klog_stream, &klog_ctx);

		rc = co_manager_kboot(handle, &b);

		/* Stop and join before anything else, in particular before the
		 * KLOAD_END that out_end reaches. */
		if (klog_thread) {
			klog_ctx.stop = 1;
			co_os_thread_join(klog_thread);
			klog_thread = NULL;
		}
		if (klog_ctx.handle) {
			co_os_manager_close(klog_ctx.handle);
			klog_ctx.handle = NULL;
		}

		if (!CO_OK(rc) || !CO_OK(b.rc)) {
			co_terminal_print("  kboot failed (rc %x / %x)\n", (int)rc, (int)b.rc);
			goto out_end;
		}
		if (b.vmx_present) {
			co_terminal_print("  REFUSED: CR4.VMXE is set -- something else has VMX\n");
			co_terminal_print("  claimed on this machine (Hyper-V role, VirtualBox, a VM).\n");
			co_terminal_print("  Clearing CR4.PGE under another hypervisor is a double\n");
			co_terminal_print("  fault. Disable it and retry.\n");
			goto out_end;
		}
		if (b.preflight_failed) {
			co_terminal_print("  PREFLIGHT REFUSED at 0x%016llx (level %d)\n",
					  b.preflight_va, b.preflight_level);
			goto out_end;
		}

		co_terminal_print("  guest cr3 0x%016llx, %lu table pages, preflight %d ok\n",
				  b.guest_cr3, b.tables, b.preflight_checked);
		co_terminal_print("  %lu world switches, %lu run yields, %lu idle yields,\n",
				  b.switches, b.run_yields, b.idle_yields);
		co_terminal_print("  %lu instructions stepped, %lu captured interrupts%s\n",
				  b.steps, b.interrupts,
				  b.hit_deadline ? "  (hit the time limit)"
				  : b.hit_limit ? "  (hit the limit)" : "");
		/*
		 * The two numbers apart, because they measure different things and
		 * only one of them is the guest. A run that is almost entirely
		 * replayed host interrupts is a run where the guest barely executed,
		 * and that used to be invisible behind a single switch count.
		 */
		/*
		 * Cooperative timer interrupts injected into a running guest.
		 * Zero means the guest was never preempted by the host -- either
		 * it always reached its idle boundary on its own, or the
		 * injection refused every time, and those two look identical
		 * from outside unless this is printed.
		 */
		co_terminal_print("  %lu cooperative ticks injected into a running guest\n",
				  b.ticks_injected);
		co_terminal_print("  %lu of those switches were the guest's own\n",
				  b.guest_switches);
		if (b.block_requests || b.block_errors)
			co_terminal_print("  %lu block transfers, %lu of them failed\n",
					  b.block_requests, b.block_errors);
		if (b.steps) {
			unsigned long i, n = (b.trace_next < 16) ? b.trace_next : 16;

			co_terminal_print("\n  last instruction addresses:\n");
			for (i = 0; i < n; i++) {
				unsigned long idx = (b.trace_next - n + i) & 15;

				co_terminal_print("    %2lu  0x%016llx\n", i, b.trace[idx]);
			}
		}
		co_terminal_print("\n");
		co_terminal_print("  ---------------- what the kernel printed ----------------\n");
		if (b.console_written == 0)
			co_terminal_print("  (nothing)\n");
		else
			co_terminal_print("%s", b.console_text);
		co_terminal_print("  --------------------------------------------------------\n");
		co_terminal_print("  %llu bytes%s\n", b.console_written,
				  (b.console_written > b.console_capacity) ? "  TRUNCATED" : "");
		co_terminal_print("\n");

		if (b.warnings) {
			unsigned long w, n = (b.warnings < 8) ? b.warnings : 8;

			co_terminal_print("  %lu kernel warning%s stepped over, as the\n",
					  b.warnings, (b.warnings == 1) ? "" : "s");
			co_terminal_print("  kernel's own #UD handler would have:\n");
			for (w = 0; w < n; w++) {
				co_terminal_print("    ud2 at 0x%016llx\n", b.warning_rip[w]);
				co_report_bug_at(pl, b.warning_rip[w]);
			}
			co_terminal_print("\n");
		}

		if (b.fixups) {
			unsigned long f, n = (b.fixups < 8) ? b.fixups : 8;
			static const char* const extype[] = {
				"none", "default", "fault", "uaccess", "?", "clear fs",
				"fpu restore", "bpf", "wrmsr", "rdmsr", "wrmsr safe",
				"rdmsr safe", "wrmsr in mce", "rdmsr in mce",
				"default mce safe", "fault mce safe", "pop reg",
				"imm reg", "fault sgx", "ucopy len", "zeropad", "eretu"
			};

			co_terminal_print("  %lu fault%s recovered from the kernel's own\n",
					  b.fixups, (b.fixups == 1) ? "" : "s");
			co_terminal_print("  exception table, as its handlers would have:\n");
			for (f = 0; f < n; f++) {
				int t = b.fixup_type[f];

				co_terminal_print("    0x%016llx  %s\n", b.fixup_rip[f],
						  (t >= 0 && t < (int)(sizeof(extype)/sizeof(extype[0])))
						  ? extype[t] : "?");
			}
			co_terminal_print("\n");
		}

		if (b.host_corrupt_field) {
			static const char* const names[] = {
				"none", "processor number", "CR0", "CR4", "CR3", "GDT base", "GDT limit",
				"IDT base", "IDT limit", "TR", "FS_BASE", "GS_BASE",
				"KERNEL_GS_BASE", "LSTAR", "STAR", "SFMASK", "EFER",
				"CS", "SS", "DS", "ES", "FS", "GS",
				"CR8 (IRQL)", "PAT", "DR7", "RFLAGS control bits",
				"CR2", "LDTR", "CSTAR", "SYSENTER_CS", "SYSENTER_ESP",
				"SYSENTER_EIP", "DR0", "DR1", "DR2", "DR3", "DR6", "XCR0"
			};
			int f = b.host_corrupt_field;
			int repaired = (f == 23 || f == 24 || f == 25); /* CR8, PAT, DR7 */

			if (repaired)
				co_terminal_print("  THE GUEST WROTE HOST STATE THE SWITCH DOES NOT CARRY.\n");
			else
				co_terminal_print("  THE HOST DID NOT COME BACK INTACT.\n");
			co_terminal_print("    %s changed across the crossing after %lu steps\n",
					  (f > 0 && f < (int)(sizeof(names)/sizeof(names[0])))
						? names[f] : "?",
					  b.host_corrupt_step);
			co_terminal_print("      was 0x%016llx\n", b.host_corrupt_expected);
			co_terminal_print("      now 0x%016llx\n", b.host_corrupt_actual);
			if (repaired) {
				co_terminal_print("    Restored to the host's value at the crossing and\n");
				co_terminal_print("    the run continued. The guest must be patched to\n");
				co_terminal_print("    stop writing it -- the repair is containment,\n");
				co_terminal_print("    not a fix.\n\n");
			} else {
				co_terminal_print("    Stopped here on purpose. Windows is running on\n");
				co_terminal_print("    that value from now on, so this is the last\n");
				co_terminal_print("    moment it can still be reported.\n\n");
			}
		}

		if (b.faulted) {
			co_terminal_print("  stopped on an exception the guest took:\n");
			co_terminal_print("    vector %llu at rip 0x%016llx\n", b.vector, b.fault_rip);
			co_terminal_print("    rdi 0x%016llx  rax 0x%016llx\n",
					  b.fault_rdi, b.fault_rax);
			co_terminal_print("    error code 0x%llx", b.error_code);
			if (b.vector == 14)
				co_terminal_print("  cr2 0x%016llx  (%s, %s)",
						  b.cr2,
						  (b.error_code & 1) ? "protection" : "not present",
						  (b.error_code & 2) ? "write" : "read");
			co_terminal_print("\n");
			if (b.fault_extype)
				co_terminal_print("    the kernel has an exception table entry\n"
						  "    for this address, of type %d, which the host\n"
						  "    does not implement -- so this is a fault the\n"
						  "    kernel expected and would have recovered from\n",
						  b.fault_extype);
		} else if (b.reached_idle && b.idle_yields) {
			co_terminal_print("\n");
			co_terminal_print("  ========================================================\n");
			co_terminal_print("  THE GUEST BOOTED AND YIELDED COOPERATIVELY.\n");
			co_terminal_print("  %lu CO_OPERATION_IDLE round trips after %lu switches --\n",
					  b.idle_yields, b.switches);
			co_terminal_print("  start_kernel and every initcall ran free (no stepping),\n");
			co_terminal_print("  the guest handed the CPU back at idle, and the host\n");
			co_terminal_print("  re-entered it repeatedly. The cooperative loop works.\n");
			co_terminal_print("  Virtual time (jiffies from the host) is the next piece.\n");
			co_terminal_print("  ========================================================\n");
		} else if (b.reached_idle) {
			co_terminal_print("\n");
			co_terminal_print("  ========================================================\n");
			co_terminal_print("  THE GUEST BOOTED THROUGH TO IDLE (via halt trap).\n");
			co_terminal_print("  It halted at 0x%016llx after %lu switches, %lu\n",
					  b.fault_rip, b.switches, b.steps);
			co_terminal_print("  instructions -- but through ud2, not a cooperative\n");
			co_terminal_print("  yield: the cooperative idle did not take.\n");
			co_terminal_print("  ========================================================\n");
		} else if (b.terminated) {
			/*
			 * The orderly ending, and the only one that means the
			 * guest chose to stop. Everything else here is the run
			 * being cut short -- a fault, a limit, an operation the
			 * host does not implement.
			 *
			 * The reasons come from the guest's reboot paths
			 * (arch/x86/kernel/reboot.c), so "poweroff" and "halt"
			 * are distinguishable, and both are distinguishable
			 * from a panic that reached halt without planning to --
			 * which still arrives as ud2 and is reported above.
			 */
			static const char* const why[] = {
				"powered off", "halted", "restarted",
				"stopped in an emergency"
			};

			co_terminal_print("\n  ========================================================\n");
			co_terminal_print("  THE GUEST SHUT ITSELF DOWN -- %s.\n",
					  b.terminate_reason < 4
						? why[b.terminate_reason]
						: "reason unknown");
			co_terminal_print("  Filesystems were flushed and unmounted by init before\n");
			co_terminal_print("  this point; the host is now free to release the guest's\n");
			co_terminal_print("  memory and close the files behind its disks.\n");
			co_terminal_print("  ========================================================\n");
		} else if (b.returned_voluntarily && b.stop_operation) {
			co_terminal_print("  the guest yielded operation %llu, which the host\n"
					  "  does not handle yet\n", b.stop_operation);
		} else if (b.hit_limit || b.hit_deadline) {
			static const char* const rn[15] = {
				"r15","r14","r13","r12","r11","r10","r9","r8",
				"rbp","rdi","rsi","rdx","rcx","rbx","rax"
			};
			int r;

			co_terminal_print("  still running after %lu switches -- stopped it on purpose\n",
					  b.switches);
			co_terminal_print("  last instruction 0x%016llx, registers:\n",
					  b.fault_rip);
			for (r = 0; r < 15; r += 3)
				co_terminal_print("    %-3s 0x%016llx   %-3s 0x%016llx   %-3s 0x%016llx\n",
						  rn[r],   b.stop_regs[r],
						  rn[r+1], b.stop_regs[r+1],
						  rn[r+2], b.stop_regs[r+2]);
		} else if (b.returned_voluntarily) {
			co_terminal_print("  the guest switched back on its own\n");
		}

		/*
		 * What the kernel wrote to printk, whether or not any console
		 * ever drained it. This is the run's own narration, read out of
		 * the ringbuffer post mortem -- the guest is stopped, its
		 * memory is still mapped, and KLOAD_END has not run yet.
		 */
		co_terminal_print("\n  ------------ the kernel's own log (tail since the live stream) ------------\n");
		co_dump_kernel_log_ex(handle, pl, &klog_since, 1);
		co_terminal_print("  -----------------------------------------------------------\n");

		/*
		 * The network rings, same arrangement as the log above: the
		 * guest is stopped, its memory is mapped until KLOAD_END, and
		 * this thread is the one that will call it.
		 */
		co_terminal_print("\n  -------------------- the network rings --------------------\n");
		co_dump_net_rings(handle, pl);
		co_terminal_print("  -----------------------------------------------------------\n");

		goto out_end;
	}

	if (!enter) {
		co_terminal_print("\n  LOADED AND VERIFIED. Not entered.\n");
		goto out_end;
	}

	co_terminal_print("\n  entering the loaded image at 0x%016llx\n\n", text_va);

	r.code_va = text_va;		/* travels inwards */
	rc = co_manager_kload_enter(handle, &r);
	if (!CO_OK(rc)) {
		co_terminal_print("  enter ioctl failed (rc %x)\n", (int)rc);
		goto out_end;
	}
	if (r.preflight_failed) {
		co_terminal_print("  PREFLIGHT REFUSED THE ENTRY at 0x%016llx (level %d)\n",
				  r.preflight_va, r.preflight_level);
		goto out_end;
	}
	if (!CO_OK(r.rc)) {
		co_terminal_print("  driver reported failure (rc %x)\n", (int)r.rc);
		goto out_end;
	}

	co_terminal_print("  guest cr3       0x%016llx  (%lu table pages)\n", r.guest_cr3, r.tables);
	co_terminal_print("  entry           0x%016llx\n", r.code_va);
	co_terminal_print("  stack           0x%016llx\n", r.guest_stack);
	co_terminal_print("  preflight       %d addresses resolved\n", r.preflight_checked);
	co_terminal_print("\n");
	co_terminal_print("  sentinel expected 0x%016llx\n", r.expected);
	co_terminal_print("  sentinel observed 0x%016llx\n", r.observed);
	co_terminal_print("\n");

	if (r.faulted)
		co_terminal_print("  FAULT: vector %llu at rip 0x%016llx, cr2 0x%016llx\n",
				  r.vector, r.fault_rip, r.cr2);
	else if (r.succeeded)
		co_terminal_print("  ENTERED THE LOADED KERNEL IMAGE AND RETURNED.\n"
				  "  Thirty-odd megabytes of vmlinux mapped at its link\n"
				  "  addresses, control transferred into it, and back.\n");
	else
		co_terminal_print("  returned, but the sentinel is wrong\n");

out_end:
	co_manager_kload_end(handle);
out:
	co_os_manager_close(handle);
	co_os_file_free(buf);

	return rc;
}
