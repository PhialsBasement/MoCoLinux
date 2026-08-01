/*
 * This source code is a part of coLinux source package.
 *
 * The code is licensed under the GPL. See the COPYING file at
 * the root directory.
 */

#ifndef __COLINUX_KERNEL_CONSOLE_H__
#define __COLINUX_KERNEL_CONSOLE_H__

#include <colinux/common/common.h>
#include <colinux/kernel/manager.h>

/*
 * The guest's interactive console: two rings in guest memory, reached through
 * the guest's own page tables. See console.c.
 */
extern co_rc_t		 co_console_init(void);
extern void		 co_console_free(void);
extern void		 co_console_set_address(unsigned long long va);
extern unsigned long long co_console_get_address(void);

extern co_rc_t co_console_pump(co_manager_t* manager,
			       const char* in, unsigned long in_size,
			       unsigned long* in_taken,
			       char* out, unsigned long out_size,
			       unsigned long* out_len);

#endif
