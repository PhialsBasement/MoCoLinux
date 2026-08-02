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
 * Print the live guest's network rings from a second process, read-only,
 * through the locked CONET_DUMP ioctl. Needs no vmlinux: the driver learned
 * the rings' address at KBOOT.
 */
extern co_rc_t co_elf_net_dump_live(void);

/*
 * Like the dump, then consume: advance tx_tail to the printed snapshot's
 * head through the validated TAKE ioctl. An explicit NEWTAIL argument asks
 * the driver for exactly that value, to test its validation.
 */
extern co_rc_t co_elf_net_take_live(const char *new_tail_arg);

/*
 * Stand in for the other end of the wire: answer ARP and ICMP echo for
 * 10.0.2.2 out of the RX ring, consuming the TX ring as it goes, until the
 * deadline. Exercises both directions with the guest's own IP stack, with no
 * slirp involved.
 */
extern co_rc_t co_elf_net_peer_live(const char *seconds_arg);

/*
 * Load an image into a guest address space in the driver, optionally entering
 * it. max_switches caps the boot monitor loop (enter == 3); 0 takes the
 * default. It exists so a run that would take the host down hard can be made
 * to stop first and report, which is the only diagnostic that survives a reset.
 */
/*
 * cobd is CO_COBD_MAX_UNITS entries or NULL; entry N is unit N's backing store,
 * NULL for a unit that is not attached. Unit 0 is what root= names.
 */
extern co_rc_t co_elf_load_into_guest(const char *filename, int enter,
				      unsigned long max_switches,
				      unsigned long batch,
				      const char *const *cobd,
				      const char *init_path,
				      unsigned long mem_mb,
				      int no_copic);
extern co_elf_symbol_t *co_get_symbol_by_name(co_elf_data_t *pl, const char *name);
extern void *co_elf_get_symbol_data(co_elf_data_t *pl, co_elf_symbol_t *symbol);
extern co_elf_addr_t co_elf_get_symbol_value(co_elf_symbol_t *symbol);

#endif
