/*
 * This source code is a part of coLinux source package.
 *
 * Dan Aloni <da-x@colinux.org>, 2003 (c)
 *
 * The code is licensed under the GPL. See the COPYING file at
 * the root directory.
 *
 */

#include <stdio.h>
#include <stdarg.h>
#include <unistd.h>
#include <stdlib.h>

#include <colinux/user/manager.h>
#include <colinux/user/daemon.h>
#include <colinux/user/debug/main.h>
#include <colinux/os/user/misc.h>

COLINUX_DEFINE_MODULE("colinux-debug-daemon");

int main(int argc, char *argv[])
{
	co_rc_t rc;

	/*
	 * One reader of the debug ring.
	 *
	 * Two of these do not each get a copy of the log -- they split it,
	 * because a record handed to one is gone. The result is a driver.log
	 * that is silently missing half of what the driver said, which defeats
	 * the one instrument every other diagnosis in this port depends on. And
	 * each copy holds a handle on the driver that blocks DriverUnload, so
	 * the second one is pure cost.
	 */
	if (!co_os_claim_single_instance("debug-daemon")) {
		co_terminal_print("colinux-debug-daemon: another instance is already"
				  " running -- exiting rather than splitting the"
				  " debug ring\n");
		return 0;
	}

	rc = co_debug_main(argc, argv);

	if (!CO_OK(rc))
		return -1;

	return 0;
}
