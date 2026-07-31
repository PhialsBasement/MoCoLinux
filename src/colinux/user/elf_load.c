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

/* How much physical memory the guest is told it has. */
#define CO_GUEST_RAM	(128ULL << 20)

/* One e820 entry: 8-byte address, 8-byte size, 4-byte type, packed to 20. */
static void co_e820_entry(unsigned char* p, unsigned long long addr,
			  unsigned long long size, unsigned int type)
{
	int i;

	for (i = 0; i < 8; i++)  p[i]      = (unsigned char)(addr >> (8 * i));
	for (i = 0; i < 8; i++)  p[8 + i]  = (unsigned char)(size >> (8 * i));
	for (i = 0; i < 4; i++)  p[16 + i] = (unsigned char)(type >> (8 * i));
}

co_rc_t co_elf_load_into_guest(const char* filename, int enter,
			       unsigned long max_switches, unsigned long batch)
{
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
		unsigned char bp[4096];
		char cmdline[256];
		co_elf_symbol_t* s_bp;
		co_elf_symbol_t* s_cl;
		co_elf_symbol_t* s_text;
		co_elf_symbol_t* s_end;
		unsigned long long ram = CO_GUEST_RAM;
		static const char* want[] = { "co_arch_start_kernel", "initial_code",
					      "start_kernel", "early_console",
					      "early_colinux_console",
					      "co_colinux_console_ring",
					      "co_colinux_guest", NULL };
		unsigned long long addr[7];
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
		co_terminal_print("    guest physical 0 is host physical 0x%llx,"
				  " %llu MB usable\n",
				  m.block_pa, m.usable_bytes >> 20);

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
		 * Guest physical N is host physical block_pa + N, so the guest's
		 * RAM starts at block_pa, not at zero -- there is no low memory
		 * and no hole, because there is no emulated machine underneath,
		 * just one contiguous allocation. The reserved entry is the
		 * region at the top holding the page tables the host built: the
		 * guest must not allocate over its own address space.
		 */
		memset(bp, 0, sizeof(bp));
		bp[0x1e8] = 2;					/* e820_entries */
		co_e820_entry(bp + 0x2d0 +  0, m.block_pa, m.usable_bytes, 1);
		co_e820_entry(bp + 0x2d0 + 20, m.block_pa + m.usable_bytes,
			      m.block_bytes - m.usable_bytes, 2);

		rc = co_manager_kload_chunk(handle, co_elf_get_symbol_value(s_bp),
					    bp, sizeof(bp), 0);
		if (!CO_OK(rc)) {
			co_terminal_print("  writing boot_params failed (rc %x)\n", (int)rc);
			goto out_end;
		}

		memset(cmdline, 0, sizeof(cmdline));
		strcpy(cmdline, "earlyprintk=colinux,keep console=earlycolinux");
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
		 * The image was loaded at guest physical (link address -
		 * __START_KERNEL_map), and guest physical is offset from host
		 * physical by the block base, so phys_base is that base. Left at
		 * zero the kernel would compute physical addresses for its own
		 * text that are 128 MB below where it actually is.
		 */
		{
			co_elf_symbol_t* s_pb = co_get_symbol_by_name(pl, "phys_base");

			if (!s_pb) {
				co_terminal_print("\n  phys_base not found\n");
				goto out_end;
			}

			rc = co_manager_kload_chunk(handle, co_elf_get_symbol_value(s_pb),
						    (unsigned char*)&m.block_pa,
						    sizeof(m.block_pa), 0);
			if (!CO_OK(rc)) {
				co_terminal_print("  writing phys_base failed (rc %x)\n", (int)rc);
				goto out_end;
			}
			co_terminal_print("    phys_base = 0x%llx\n", m.block_pa);
		}

		co_terminal_print("    e820: 0x%llx-0x%llx usable (%llu MB),"
				  " 0x%llx-0x%llx reserved (%llu MB)\n",
				  m.block_pa, m.block_pa + m.usable_bytes,
				  m.usable_bytes >> 20,
				  m.block_pa + m.usable_bytes,
				  m.block_pa + m.block_bytes,
				  (m.block_bytes - m.usable_bytes) >> 20);
		co_terminal_print("    cmdline: %s\n", cmdline);

		b.entry_va           = addr[0];
		b.initial_code_va    = addr[1];
		b.start_kernel_va    = addr[2];
		b.early_console_va   = addr[3];
		b.colinux_console_va = addr[4];
		b.ring_symbol_va     = addr[5];
		b.guest_flag_va      = addr[6];
		/*
		 * Stepped, always, for now. The guest runs with interrupts disabled
		 * through all of setup_arch, so nothing else can take control back
		 * from it, and an unbounded guest takes the host with it.
		 */
		b.step         = 1;
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
		b.batch        = batch ? batch : 256;

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

		co_terminal_print("\n  booting:\n");
		co_terminal_print("    stopping after %d world switches%s\n",
				  b.max_switches,
				  max_switches ? "  (--max-switches)" : "");
		co_terminal_print("    %d instructions stepped per crossing%s\n",
				  b.batch, batch ? "  (--batch)" : "");
		for (i = 0; want[i]; i++)
			co_terminal_print("    %-24s 0x%016llx\n", want[i], addr[i]);

		co_terminal_print("\n  entering co_arch_start_kernel with interrupts enabled,\n");
		co_terminal_print("  initial_code pointed at start_kernel, the early console\n");
		co_terminal_print("  wired, and host interrupts forwarded back to Windows\n\n");

		rc = co_manager_kboot(handle, &b);
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
		co_terminal_print("  %lu world switches, %lu instructions stepped, %lu interrupts%s\n",
				  b.switches, b.steps, b.interrupts,
				  b.hit_limit ? "  (hit the limit)" : "");
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

		if (b.host_corrupt_field) {
			static const char* const names[] = {
				"none", "processor number", "CR0", "CR4", "CR3", "GDT base", "GDT limit",
				"IDT base", "IDT limit", "TR", "FS_BASE", "GS_BASE",
				"KERNEL_GS_BASE", "LSTAR", "STAR", "SFMASK", "EFER",
				"CS", "SS"
			};
			int f = b.host_corrupt_field;

			co_terminal_print("  THE HOST DID NOT COME BACK INTACT.\n");
			co_terminal_print("    %s changed across the crossing after %lu steps\n",
					  (f > 0 && f < (int)(sizeof(names)/sizeof(names[0])))
						? names[f] : "?",
					  b.host_corrupt_step);
			co_terminal_print("      was 0x%016llx\n", b.host_corrupt_expected);
			co_terminal_print("      now 0x%016llx\n", b.host_corrupt_actual);
			co_terminal_print("    Stopped here on purpose. Windows is running on\n");
			co_terminal_print("    that value from now on, so this is the last\n");
			co_terminal_print("    moment it can still be reported.\n\n");
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
		} else if (b.hit_limit) {
			co_terminal_print("  still running after %lu switches -- stopped it on purpose\n",
					  b.switches);
		} else if (b.returned_voluntarily) {
			co_terminal_print("  the guest switched back on its own\n");
		}

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
