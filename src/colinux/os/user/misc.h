/*
 * This source code is a part of coLinux source package.
 *
 * Dan Aloni <da-x@colinux.org>, 2003 (c)
 *
 * The code is licensed under the GPL. See the COPYING file at
 * the root directory.
 *
 */

#ifndef __COLINUX_OS_USER_MISC_H__
#define __COLINUX_OS_USER_MISC_H__

#if defined __cplusplus
extern "C" {
#endif

#include <colinux/common/common.h>

typedef void (*co_terminal_print_hook_func_t)(char* str);

typedef enum {
	CO_TERM_COLOR_YELLOW = 1,
	CO_TERM_COLOR_WHITE  = 2,
} co_terminal_color_t;

extern void co_terminal_print(const char* format, ...)
	__attribute__ ((format (printf, 1, 2)));
extern void co_terminal_print_color(co_terminal_color_t color, const char* format, ...)
	__attribute__ ((format (printf, 2, 3)));
extern void co_set_terminal_print_hook(co_terminal_print_hook_func_t func);
extern void co_process_high_priority_set(void);
/*
 * Give the processor up for a while. A poll loop against a live guest needs
 * this and there was no OS-independent way to ask for it -- the console
 * server is in the winnt tree and reaches straight for Sleep().
 */
extern void co_os_user_msleep(unsigned long msec);
/*
 * Refuse to be the second copy of yourself, machine-wide.
 *
 * Every long-lived daemon here holds a handle on the driver, and a second copy
 * is not a harmless duplicate but a competitor: two slirp bridges both consume
 * the guest's TX ring and both inject into its RX ring with nothing arbitrating
 * between them, two consoles fight over one port while the loser keeps its
 * handle, two debug daemons split the debug ring so the log silently receives
 * half of what the driver said. Each extra handle also blocks DriverUnload, so
 * the only way to clear one has been to reboot.
 *
 * It cannot be left to whoever starts them. The transfer agent retries a
 * command it believes timed out, which launches a daemon up to three times on
 * its own, and a launcher that never returns invites a second attempt by hand.
 * The guard belongs in the process, where neither mistake reaches it.
 *
 * Returns PFALSE if another instance already holds the name; the caller should
 * say so and exit without opening the driver.
 */
extern bool_t co_os_claim_single_instance(const char* name);
extern int co_udp_socket_connect(const char* addr, unsigned short int port);
extern int co_udp_socket_send(int sock, const char* buffer, unsigned long size);
extern void co_udp_socket_close(int sock);

#if defined __cplusplus
}
#endif

#endif /* __COLINUX_OS_USER_MISC_H__ */
