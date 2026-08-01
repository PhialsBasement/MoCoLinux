/*
 * This source code is a part of coLinux source package.
 *
 * Dan Aloni <da-x@colinux.org>, 2003 (c)
 *
 * The code is licensed under the GPL. See the COPYING file at
 * the root directory.
 *
 */

#ifndef __COLINUX_USER_ELF_LOAD_H__
#define __COLINUX_USER_ELF_LOAD_H__

#include <colinux/common/common.h>

typedef	struct co_elf_data co_elf_data_t;
typedef	struct co_elf_symbol co_elf_symbol_t;

/*
 * Addresses and file offsets out of the image are 64-bit regardless of host.
 * Windows is LLP64, so `unsigned long` is 4 bytes there and would silently
 * truncate every kernel address above 4 GB -- which on x86-64 is all of them.
 */
typedef unsigned long long co_elf_addr_t;
typedef unsigned long long co_elf_off_t;

struct co_daemon;

extern co_rc_t co_elf_image_read(co_elf_data_t **pl, void *elf_buf, unsigned long size);
extern co_rc_t co_elf_image_load(struct co_daemon *daemon);

/*
 * Enumerate the image without loading it. An x86-64 bring-up aid: the parser
 * had to grow an ELF64 path, and the only place the LLP64 truncation hazards
 * are real is the Windows host, so the check has to run there.
 */
extern co_rc_t co_elf_dump(const char *filename);

/*
 * Load an image into a guest address space in the driver, optionally entering
 * it. max_switches caps the boot monitor loop (enter == 3); 0 takes the
 * default. It exists so a run that would take the host down hard can be made
 * to stop first and report, which is the only diagnostic that survives a reset.
 */
extern co_rc_t co_elf_load_into_guest(const char *filename, int enter,
				      unsigned long max_switches,
				      unsigned long batch,
				      const char *cobd0);
extern co_elf_symbol_t *co_get_symbol_by_name(co_elf_data_t *pl, const char *name);
extern void *co_elf_get_symbol_data(co_elf_data_t *pl, co_elf_symbol_t *symbol);
extern co_elf_addr_t co_elf_get_symbol_value(co_elf_symbol_t *symbol);

#endif
