/*
 * This source code is a part of coLinux source package.
 *
 * Dan Aloni <da-x@colinux.org>, 2003 (c)
 *
 * The code is licensed under the GPL. See the COPYING file at
 * the root directory.
 *
 */

#include "../time.h"

#include <colinux/common/common.h>
#include <colinux/os/kernel/time.h>
#include <colinux/os/timer.h>

unsigned long co_os_get_time()
{
	LARGE_INTEGER CurrentTime;

	KeQuerySystemTime(&CurrentTime);

	return windows_time_to_unix_time(CurrentTime);
}

unsigned long long co_os_monotonic_100ns(void)
{
	/*
	 * KeQueryInterruptTime: 100 ns since boot, monotonic, safe at any
	 * IRQL, and cheap -- it reads the shared-data tick, it does not take
	 * the HAL's timer path the way KeQueryPerformanceCounter can.
	 */
	return KeQueryInterruptTime();
}

void co_os_get_timestamp(co_timestamp_t *dts)
{
	co_os_get_timestamp_freq(dts, NULL);
}

void co_os_get_timestamp_freq(co_timestamp_t *dts, co_timestamp_t *freq)
{
	LARGE_INTEGER PerformanceFrequency;
	LARGE_INTEGER PerformanceCounter;

	PerformanceCounter = KeQueryPerformanceCounter(&PerformanceFrequency);

	dts->high = PerformanceCounter.HighPart;
	dts->low = PerformanceCounter.LowPart;

	if (freq) {
		freq->high = PerformanceFrequency.HighPart;
		freq->low = PerformanceFrequency.LowPart;
	}
}

unsigned long co_os_get_cpu_khz(void)
{
	NTSTATUS status;
	static ULONG mhz = 0;
	static RTL_QUERY_REGISTRY_TABLE query[2] = {{
		.Flags = RTL_QUERY_REGISTRY_REQUIRED
		       | RTL_QUERY_REGISTRY_DIRECT
		       | RTL_QUERY_REGISTRY_NOEXPAND,
		.Name = L"~MHz",
		.EntryContext = &mhz,
		.DefaultType = REG_DWORD,
		}};

	if (!mhz) {
		status = RtlQueryRegistryValues(
				RTL_REGISTRY_ABSOLUTE,
				L"\\Registry\\MACHINE\\HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
		    		query, NULL, NULL);

		if (status != STATUS_SUCCESS)
			co_debug("getting MHz failed %x", (int)status);
	}

	return(1000*mhz);
}
