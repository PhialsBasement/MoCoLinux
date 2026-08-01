/*
 * This source code is a part of coLinux source package.
 *
 * The code is licensed under the GPL. See the COPYING file at
 * the root directory.
 */

#ifndef __COLINUX_OS_WINNT_USER_DAEMON_CONSOLE_H__
#define __COLINUX_OS_WINNT_USER_DAEMON_CONSOLE_H__

#include <colinux/common/common.h>

/* Serve the guest's console on a TCP port. Does not return. See console.c. */
extern co_rc_t co_winnt_console_server(unsigned short port);

#endif
