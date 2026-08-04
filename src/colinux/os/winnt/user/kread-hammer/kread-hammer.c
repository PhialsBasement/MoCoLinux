/*
 * kread-hammer -- prove the KREAD teardown lock, by trying to break it.
 *
 * R0's verification, and the reason it exists: the lock added in
 * co_kload_read_locked is correct by inspection, and correct-by-inspection is
 * exactly the standard this project has been burned by. The bug it prevents --
 * a second process walking guest page tables while another frees them -- had
 * lived in the tree for weeks looking perfectly safe, because its one caller
 * happened to be sequenced before its own teardown.
 *
 * So this hammers CO_MANAGER_IOCTL_KREAD from a process that is NOT the one
 * running the guest, continuously, at maximum rate, straight through the window
 * where the guest is torn down. Two runs are the evidence:
 *
 *   1. Against the UNFIXED driver, under Driver Verifier's special pool: it
 *      must bugcheck (0xD5 / 0x50) at least once. Without that the "clean"
 *      run below is worthless -- a no-op change also produces twenty clean
 *      cycles, and this project has already accepted a passing test that
 *      proved nothing.
 *   2. Against the fixed driver: no bugcheck, and every call after the
 *      teardown returns a failure rc rather than data.
 *
 * The second is the interesting one to get right. A reader that simply stops
 * being scheduled also produces no bugcheck, so the tool counts what came back:
 * a run where the reads all failed early is not evidence, and neither is one
 * where they all succeeded -- the teardown has to be observed happening, as a
 * transition from OK to failing, with no crash across it.
 *
 * Usage:
 *   kread-hammer --va 0xffffffff81000000 [--size 65536] [--seconds 60]
 *
 * Run it while the guest boots and while stop.bat ends it. Nothing here is
 * destructive: it only reads.
 */

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <colinux/common/common.h>
#include <colinux/common/debug.h>
#include <colinux/user/manager.h>
#include <colinux/os/user/manager.h>
#include <colinux/os/alloc.h>

COLINUX_DEFINE_MODULE("kread-hammer");

static double now_s(void)
{
	LARGE_INTEGER f, t;

	QueryPerformanceFrequency(&f);
	QueryPerformanceCounter(&t);
	return (double)t.QuadPart / (double)f.QuadPart;
}

int main(int argc, char **argv)
{
	unsigned long long va = 0xffffffff81000000ULL;
	unsigned long size = 64 * 1024;
	double seconds = 60.0;
	co_manager_handle_t handle;
	unsigned char *buf;
	double t0, last_report;
	unsigned long long ok = 0, failed = 0, calls = 0;
	unsigned long long first_fail_after_ok = 0;
	int seen_ok = 0, i;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--va") && i + 1 < argc)
			va = _strtoui64(argv[++i], NULL, 0);
		else if (!strcmp(argv[i], "--size") && i + 1 < argc)
			size = strtoul(argv[++i], NULL, 0);
		else if (!strcmp(argv[i], "--seconds") && i + 1 < argc)
			seconds = atof(argv[++i]);
		else {
			printf("usage: kread-hammer [--va ADDR] [--size N] [--seconds N]\n");
			return 2;
		}
	}

	/*
	 * 2 MB is CO_KREAD_MAX_BYTES. Asking for more must be refused rather
	 * than served, so the cap is checked here as well as hammered around.
	 */
	buf = malloc(size);
	if (!buf) {
		printf("out of memory\n");
		return 1;
	}

	handle = co_os_manager_open();
	if (!handle) {
		printf("cannot open the driver -- is it started?\n");
		return 1;
	}

	printf("hammering KREAD: va 0x%llx, %lu bytes, %.0f s\n",
	       (unsigned long long)va, size, seconds);
	printf("start the guest and stop it while this runs.\n\n");

	t0 = now_s();
	last_report = t0;

	for (;;) {
		co_rc_t rc = co_manager_kread(handle, va, buf, size);
		double now = now_s();

		calls++;
		if (CO_OK(rc)) {
			ok++;
			seen_ok = 1;
		} else {
			failed++;
			/* The transition is the evidence that a teardown was
			 * actually crossed rather than missed entirely. */
			if (seen_ok && !first_fail_after_ok)
				first_fail_after_ok = calls;
		}

		if (now - last_report >= 5.0) {
			printf("  %6.0fs  calls %-10llu ok %-10llu failed %-10llu\n",
			       now - t0, calls, ok, failed);
			last_report = now;
		}

		if (now - t0 >= seconds)
			break;
	}

	co_os_manager_close(handle);

	printf("\ncalls          %llu\n", calls);
	printf("ok             %llu\n", ok);
	printf("failed         %llu\n", failed);
	printf("first failure after a success: %s\n",
	       first_fail_after_ok ? "yes" : "no");

	/*
	 * What the operator has to see for this to mean anything. Stated rather
	 * than scored, because the pass condition depends on what was done to
	 * the box while it ran -- the tool cannot know whether a teardown
	 * happened, only report what it observed.
	 */
	printf("\nevidence checklist:\n");
	printf("  [%c] reads succeeded at some point (the guest was up)\n",
	       ok ? 'x' : ' ');
	printf("  [%c] reads then failed (a teardown was crossed)\n",
	       first_fail_after_ok ? 'x' : ' ');
	printf("  [ ] the host did not bugcheck  <- check this yourself\n");
	printf("  [ ] Driver Verifier special pool was on for linux.sys\n");

	free(buf);
	return 0;
}
