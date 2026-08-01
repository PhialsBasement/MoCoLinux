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
extern int co_udp_socket_connect(const char* addr, unsigned short int port);
extern int co_udp_socket_send(int sock, const char* buffer, unsigned long size);
extern void co_udp_socket_close(int sock);

#if defined __cplusplus
}
#endif

#endif /* __COLINUX_OS_USER_MISC_H__ */
