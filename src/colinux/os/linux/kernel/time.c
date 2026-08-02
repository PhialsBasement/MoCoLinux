/*
 * This source code is a part of coLinux source package.
 *
 * The code is licensed under the GPL. See the COPYING file at
 * the root directory.
 */

#include <colinux/common/common.h>
#include <colinux/os/kernel/time.h>

unsigned long co_os_timer_resolution_acquire(void)
{
	/*
	 * Nothing to claim on a Linux host: hrtimers already give sub-tick
	 * sleeps, so co_os_msleep means what it says and there is no global
	 * clock resolution to raise. Returning zero says "not applicable"
	 * rather than reporting a resolution that was never granted.
	 *
	 * The Windows side of this is not cosmetic -- see
	 * os/winnt/kernel/lowlevel/timer.c. There every sleep is rounded up to
	 * the system tick, 15.6 ms by default, which put a floor under every
	 * round trip the guest made and was most of the reason its network was
	 * slow.
	 */
	return 0;
}

void co_os_timer_resolution_release(void)
{
}
