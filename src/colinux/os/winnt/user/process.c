/*
 * This source code is a part of coLinux source package.
 *
 * Henry Nestler, 2007 (c)
 *
 * The code is licensed under the GPL. See the COPYING file at
 * the root directory.
 *
 */

#include <windows.h>

#include <colinux/os/user/misc.h>
#include <colinux/os/winnt/user/misc.h>

void co_process_high_priority_set(void)
{
	if (!SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS))
		co_terminal_print_last_error("SetPriorityClass");
}

void co_os_user_msleep(unsigned long msec)
{
	Sleep(msec);
}
