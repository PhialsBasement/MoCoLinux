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
