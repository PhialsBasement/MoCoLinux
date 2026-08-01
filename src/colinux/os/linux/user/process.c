/*
 * This source code is a part of coLinux source package.
 *
 * Henry Nestler, 2007 (c)
 *
 * The code is licensed under the GPL. See the COPYING file at
 * the root directory.
 *
 */

#include <unistd.h>

#include <colinux/os/user/misc.h>

void co_process_high_priority_set(void)
{
	/* nothing */
}

void co_os_user_msleep(unsigned long msec)
{
	usleep(msec * 1000);
}
