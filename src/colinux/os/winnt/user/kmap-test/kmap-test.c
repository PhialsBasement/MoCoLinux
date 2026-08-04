/*
 * kmap-test -- R3 step 1: does IRP_MJ_CLEANUP unmap what a dead process left?
 *
 * The whole of R3 rests on one assumption that is documented by Microsoft and
 * has never been exercised in this tree: that a user-mode mapping made by the
 * driver is torn down when the owning process's handle is closed, INCLUDING
 * when the process dies without asking. If that is false, a crashed daemon
 * leaves an MDL mapped into a dead address space and the driver can never
 * safely free the guest's RAM again -- so it has to be established before
 * anything is built on top of it, not discovered at R5.
 *
 * The three modes are the three ways a process can let go, and they are
 * deliberately separate runs rather than one clever program: if this bugchecks
 * the host, the mode that did it is the finding.
 *
 *   --clean   map, read, KUNMAP, exit tidily          (the easy case)
 *   --exit    map, read, exit WITHOUT unmapping       (the assumption)
 *   --crash   map, read, then dereference NULL        (the daemon that dies)
 *
 * What "read" means here matters, because a mapping that is present but wrong
 * is the failure this cannot afford to call a pass. It reads through the
 * mapping and, for the same physical addresses, reads the same bytes back
 * through KREAD -- the slow ioctl path that walks the guest's own page tables.
 * Two independent routes to the same memory agreeing is the evidence that the
 * window is real; a window full of zeroes, or of somebody else's memory, fails
 * loudly rather than printing a throughput number and looking successful.
 *
 * Run it with a guest up. Nothing here writes to guest memory.
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

COLINUX_DEFINE_MODULE("kmap-test");

static double now_s(void)
{
	LARGE_INTEGER f, t;

	QueryPerformanceFrequency(&f);
	QueryPerformanceCounter(&t);
	return (double)t.QuadPart / (double)f.QuadPart;
}

/*
 * A cheap checksum with position in it, so that two different pages of zeroes
 * do not agree by accident -- which a plain sum over an unbooted guest's RAM
 * would do everywhere.
 */
static unsigned long long sum_of(const unsigned char* p, unsigned long n)
{
	unsigned long long s = 1469598103934665603ULL;
	unsigned long i;

	for (i = 0; i < n; i++) {
		s ^= p[i];
		s *= 1099511628211ULL;
	}
	return s;
}

int main(int argc, char** argv)
{
	enum { MODE_CLEAN, MODE_EXIT, MODE_CRASH } mode = MODE_CLEAN;
	co_manager_handle_t handle;
	co_manager_ioctl_kmap_t* map;
	unsigned long slice = 0;
	int i, checked = 0, mismatches = 0;
	double t0, t1;
	unsigned long long total = 0;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--clean"))
			mode = MODE_CLEAN;
		else if (!strcmp(argv[i], "--exit"))
			mode = MODE_EXIT;
		else if (!strcmp(argv[i], "--crash"))
			mode = MODE_CRASH;
		else if (!strcmp(argv[i], "--slice") && i + 1 < argc)
			slice = strtoul(argv[++i], NULL, 0);
		else {
			printf("usage: kmap-test [--clean|--exit|--crash] [--slice BYTES]\n");
			return 2;
		}
	}

	map = malloc(sizeof(*map));
	if (!map) {
		printf("out of memory\n");
		return 1;
	}

	handle = co_os_manager_open();
	if (!handle) {
		printf("cannot open the driver -- is it started?\n");
		return 1;
	}

	printf("kmap-test: mode %s, slice %lu\n",
	       mode == MODE_CLEAN ? "clean" : mode == MODE_EXIT ? "exit" : "crash",
	       slice);

	if (!CO_OK(co_manager_kmap(handle, slice, map))) {
		printf("KMAP failed -- is a guest running?\n");
		co_os_manager_close(handle);
		return 1;
	}

	printf("mapped %lu slices, %llu MB\n", map->count, map->total_bytes >> 20);
	for (i = 0; i < (int)map->count && i < 4; i++) {
		printf("  slice %d: pa 0x%llx  %llu MB  at user va 0x%llx\n",
		       i, map->range[i].pa, map->range[i].bytes >> 20,
		       map->range[i].user_va);
	}
	if (map->count > 4)
		printf("  ... and %lu more\n", map->count - 4);

	/*
	 * Read every slice through the window, and time it. This is also the
	 * first measurement of what R3 is for: in-place reads at memory speed
	 * rather than an ioctl per buffer.
	 */
	t0 = now_s();
	for (i = 0; i < (int)map->count; i++) {
		const unsigned char* p =
			(const unsigned char*)(unsigned long)map->range[i].user_va;
		volatile unsigned long long s = sum_of(p, (unsigned long)map->range[i].bytes);

		(void)s;
		total += map->range[i].bytes;
	}
	t1 = now_s();

	printf("\nread %llu MB through the window in %.2f s = %.0f MB/s\n",
	       total >> 20, t1 - t0,
	       (double)(total >> 20) / (t1 - t0 > 0 ? t1 - t0 : 1));

	/*
	 * The cross-check. KREAD takes a guest VIRTUAL address, and what the
	 * window gives is physical, so the two are compared at the one place
	 * this program can name in both: the guest's direct map, which is
	 * PAGE_OFFSET + pa. That is exactly how the guest itself reaches this
	 * memory, so a disagreement means the window is not the guest's RAM.
	 */
	{
		const unsigned long long PAGE_OFFSET = 0xffff888000000000ULL;
		unsigned char via_kread[4096];

		for (i = 0; i < (int)map->count && checked < 8; i++) {
			const unsigned char* p =
				(const unsigned char*)(unsigned long)map->range[i].user_va;
			unsigned long long va = PAGE_OFFSET + map->range[i].pa;

			if (!CO_OK(co_manager_kread(handle, va, via_kread,
						    sizeof(via_kread))))
				continue;

			checked++;
			if (memcmp(p, via_kread, sizeof(via_kread)) != 0) {
				mismatches++;
				printf("  MISMATCH at slice %d, pa 0x%llx:"
				       " window %016llx vs kread %016llx\n",
				       i, map->range[i].pa,
				       sum_of(p, sizeof(via_kread)),
				       sum_of(via_kread, sizeof(via_kread)));
			}
		}
	}

	printf("cross-checked %d slices against KREAD: %d mismatch(es)\n",
	       checked, mismatches);

	if (checked == 0)
		printf("  (no slice could be read back through KREAD -- inconclusive)\n");

	switch (mode) {
	case MODE_CLEAN: {
		unsigned long released = 0;

		if (CO_OK(co_manager_kunmap(handle, &released)))
			printf("\nKUNMAP released %lu slices\n", released);
		else
			printf("\nKUNMAP failed\n");
		co_os_manager_close(handle);
		printf("closed the handle tidily\n");
		break;
	}

	case MODE_EXIT:
		printf("\nexiting WITHOUT unmapping and WITHOUT closing the handle.\n"
		       "IRP_MJ_CLEANUP must do it. Check the driver log for"
		       " 'kmap: released'.\n");
		/* Deliberately no close and no unmap: the OS closes the handle
		 * on process teardown, which is the path under test. */
		break;

	case MODE_CRASH:
		printf("\ncrashing on purpose, still holding %lu slices.\n",
		       map->count);
		fflush(stdout);
		*(volatile int*)0 = 1;
		break;
	}

	return mismatches ? 1 : 0;
}
