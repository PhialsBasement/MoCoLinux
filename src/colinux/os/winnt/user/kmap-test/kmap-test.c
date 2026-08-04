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
	enum { MODE_CLEAN, MODE_EXIT, MODE_CRASH, MODE_STARVE } mode = MODE_CLEAN;
	co_manager_handle_t handle;
	co_manager_ioctl_kmap_t* map;
	unsigned long slice = 0;
	int i, checked = 0, mismatches = 0;
	double t0, t1;
	unsigned long long total = 0;
	static volatile unsigned long long sink;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--clean"))
			mode = MODE_CLEAN;
		else if (!strcmp(argv[i], "--exit"))
			mode = MODE_EXIT;
		else if (!strcmp(argv[i], "--crash"))
			mode = MODE_CRASH;
		else if (!strcmp(argv[i], "--starve"))
			mode = MODE_STARVE;
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
	       mode == MODE_CLEAN ? "clean" : mode == MODE_EXIT ? "exit" :
	       mode == MODE_CRASH ? "crash" : "starve", slice);

	/*
	 * The failure path, exercised rather than assumed.
	 *
	 * The driver probes for free user address space before every map,
	 * because MmMapLockedPagesSpecifyCache RAISES on failure and cannot be
	 * caught in this toolchain -- so the probe is the only thing standing
	 * between an exhausted address space and a bugcheck. Untested code is
	 * the thing this project keeps being bitten by, so this mode reserves
	 * essentially the whole user address space first and then asks for a
	 * gigabyte of windows.
	 *
	 * The pass condition is a clean refusal and a live host. A bugcheck
	 * here means the probe does not work; a SUCCESS here means the
	 * starvation did not take, and the test proved nothing.
	 */
	if (mode == MODE_STARVE) {
		unsigned long long held = 0;
		void* p;

		printf("reserving user address space until it runs out...\n");
		for (;;) {
			p = VirtualAlloc(NULL, 256ULL << 20, MEM_RESERVE,
					 PAGE_READWRITE);
			if (!p)
				break;
			held += 256ULL << 20;
			if (held > (200ULL << 30))
				break;
		}
		printf("reserved %llu GB; largest remaining hole is now small\n",
		       held >> 30);

		/* Squeeze the rest out in smaller pieces, so nothing 8 MB wide
		 * is left -- an 8 MB slice is what the driver will ask for. */
		for (;;) {
			p = VirtualAlloc(NULL, 8ULL << 20, MEM_RESERVE,
					 PAGE_READWRITE);
			if (!p)
				break;
			held += 8ULL << 20;
		}
		printf("no 8 MB hole remains. asking the driver to map anyway.\n");

		if (CO_OK(co_manager_kmap(handle, slice, map))) {
			printf("\nFAIL: KMAP SUCCEEDED with no address space left"
			       " -- the starvation did not take, so this run\n"
			       "      proves nothing about the probe.\n");
			co_manager_kunmap(handle, NULL);
			co_os_manager_close(handle);
			return 1;
		}

		printf("\nPASS: KMAP refused cleanly and the host is alive.\n"
		       "      That is the probe working; without it this is a"
		       " bugcheck 0x1E/0x7E.\n");
		co_os_manager_close(handle);
		return 0;
	}

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
	 * Read bandwidth, measured as reading rather than as hashing.
	 *
	 * The first version of this timed sum_of() -- a byte-at-a-time FNV --
	 * over every slice and reported 819 MB/s, which is SSD speed and
	 * obviously wrong for DDR3: what it measured was the hash, not the
	 * memory. One multiply and one xor per BYTE cannot keep up with a
	 * memory bus, so the number said more about the loop than the mapping.
	 * (R1 made exactly this mistake with a CRC and understated readback by
	 * 2.5x. Twice is a pattern: never put a per-byte computation inside a
	 * span meant to measure a transfer.)
	 *
	 * So the timed loop now does the cheapest thing that still forces every
	 * cache line to be fetched -- a 64-bit accumulate, one operation per
	 * eight bytes -- and the checksum moved out of it. 1 GB does not fit in
	 * this part's 6 MB of L3, so this is genuine DRAM bandwidth rather than
	 * a cache measurement.
	 */
	t0 = now_s();
	for (i = 0; i < (int)map->count; i++) {
		const unsigned long long* q =
			(const unsigned long long*)(unsigned long)map->range[i].user_va;
		unsigned long long n = map->range[i].bytes / sizeof(*q);
		unsigned long long acc = 0, j;

		for (j = 0; j < n; j++)
			acc += q[j];

		/* Consumed, so the loop above cannot be optimised away. */
		sink += acc;
		total += map->range[i].bytes;
	}
	t1 = now_s();

	printf("\nread %llu MB through the window in %.3f s = %.0f MB/s\n",
	       total >> 20, t1 - t0,
	       (double)(total >> 20) / (t1 - t0 > 0 ? t1 - t0 : 1));

	/*
	 * The control, and the load-bearing comparison of this whole rung.
	 *
	 * KREAD reaches the same memory through an ioctl and a page-table walk
	 * per call. If the window were somehow not a real mapping -- if it had
	 * quietly degraded to copying, say -- the two would come out alike. A
	 * large ratio is the evidence that nothing is being copied.
	 */
	{
		static const unsigned long sizes[] = { 4096, 65536 };
		const unsigned long long PAGE_OFFSET = 0xffff888000000000ULL;
		double win_mbs = (double)(total >> 20) / (t1 - t0);
		unsigned char* ctl = malloc(65536);
		int s;

		if (ctl) {
			for (s = 0; s < 2; s++) {
				unsigned long	   chunk = sizes[s];
				unsigned long long ctl_bytes = 0;
				/* 64 MB per size, so the timer has something to
				 * measure -- 4 ms of samples was noise. */
				unsigned long	   iters = (64UL << 20) / chunk;
				double		   c0, c1;
				unsigned long	   k;

				c0 = now_s();
				for (k = 0; k < iters; k++) {
					unsigned long long va =
						PAGE_OFFSET + map->range[0].pa +
						(unsigned long long)(k % 128) * chunk;

					if (!CO_OK(co_manager_kread(handle, va, ctl, chunk)))
						break;
					ctl_bytes += chunk;
				}
				c1 = now_s();

				if (ctl_bytes && c1 > c0) {
					double ctl_mbs = (double)(ctl_bytes >> 20) / (c1 - c0);

					printf("KREAD control @ %5lu B: %.0f MB/s"
					       "  (%.1f us/call)  -> window %.0fx faster\n",
					       chunk, ctl_mbs,
					       (c1 - c0) * 1e6 / (double)(ctl_bytes / chunk),
					       ctl_mbs > 0 ? win_mbs / ctl_mbs : 0.0);
				}
			}
			free(ctl);
		}
	}

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
		/*
		 * No crash dialog. The fault is deliberate, and Windows Error
		 * Reporting otherwise puts up "kmap-test.exe has stopped
		 * working" and keeps the dying process alive until a human
		 * clicks it -- which blocks any script running this, and, worse,
		 * means the handle is NOT yet closed, so the very cleanup path
		 * under test has not run. Twice this looked like a hang.
		 */
		SetErrorMode(SEM_NOGPFAULTERRORBOX | SEM_FAILCRITICALERRORS);
		*(volatile int*)0 = 1;
		break;
	}

	return mismatches ? 1 : 0;
}
