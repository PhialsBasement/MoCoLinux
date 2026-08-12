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

#ifndef QUOTA_LIMITS_HARDWS_MIN_ENABLE
#define QUOTA_LIMITS_HARDWS_MIN_ENABLE	0x1
#define QUOTA_LIMITS_HARDWS_MIN_DISABLE	0x2
#define QUOTA_LIMITS_HARDWS_MAX_ENABLE	0x4
#define QUOTA_LIMITS_HARDWS_MAX_DISABLE	0x8
#endif

/*
 * Lift the working-set quota before mapping guest RAM.
 *
 * MmMapLockedPagesSpecifyCache(UserMode) charges the mapped pages against the
 * CALLING PROCESS's working set, and a process's default working-set maximum
 * is a few hundred MB -- nothing to do with how much RAM the machine has. The
 * project's "~1.5-2 GB of mappings kills the host" ceiling was this quota:
 * the raise arrives with the guard broken, so it presents as bugcheck 0x1E
 * and reads like a hardware wall. It is a per-process default, and this is
 * the call that moves it.
 *
 * Ex first (hard minimum, soft maximum), plain SetProcessWorkingSetSize as
 * the fallback for XP where Ex does not exist. Returns what was requested on
 * success, 0 when every form was refused.
 */
/* SeIncreaseWorkingSetPrivilege arrives DISABLED in every token, and the
 * working-set call fails rather than enabling it -- the daemon's first
 * deployment proved that on hardware. Enable it explicitly. */
static void enable_privilege(const char *name)
{
	HANDLE token;
	TOKEN_PRIVILEGES tp;

	if (!OpenProcessToken(GetCurrentProcess(),
			      TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token))
		return;
	tp.PrivilegeCount = 1;
	tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
	if (LookupPrivilegeValueA(NULL, name, &tp.Privileges[0].Luid))
		AdjustTokenPrivileges(token, FALSE, &tp, 0, NULL, NULL);
	CloseHandle(token);
}

static int raise_ws_once(unsigned long long want)
{
	typedef BOOL (WINAPI *setws_ex_t)(HANDLE, SIZE_T, SIZE_T, DWORD);
	setws_ex_t setws_ex = (setws_ex_t)GetProcAddress(
		GetModuleHandleA("kernel32.dll"), "SetProcessWorkingSetSizeEx");
	SIZE_T want_min = (SIZE_T)(want + (256ULL << 20));
	SIZE_T want_max = want_min + (256ULL << 20);

	/* Soft limits only: a hard minimum RESERVES pages system-wide and
	 * starved everything else the one time it was tried. */
	if (setws_ex &&
	    setws_ex(GetCurrentProcess(), want_min, want_max,
		     QUOTA_LIMITS_HARDWS_MIN_DISABLE |
		     QUOTA_LIMITS_HARDWS_MAX_DISABLE))
		return 1;
	if (SetProcessWorkingSetSize(GetCurrentProcess(), want_min, want_max))
		return 1;
	return 0;
}

static unsigned long long raise_ws_for(unsigned long long bytes)
{
	unsigned long long want = bytes;

	enable_privilege("SeIncreaseWorkingSetPrivilege");
	enable_privilege("SeIncreaseQuotaPrivilege");

	while (want >= (512ULL << 20)) {
		if (raise_ws_once(want))
			return want;
		printf("working set: %llu MB refused (%lu); halving\n",
		       want >> 20, (unsigned long)GetLastError());
		want >>= 1;
	}
	return 0;
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

/*
 * --grow: the ratchet, proven end to end in a disposable process.
 *
 * Maps guest RAM slice by slice through KMAP_RANGE -- the daemon's own
 * pattern -- and raises the working set in +512 MB steps as the mapped total
 * approaches the current grant. The wholesale request for all of guest RAM
 * is refused with ERROR_NO_SYSTEM_RESOURCES (1450) because the minimum is
 * checked against currently available pages; the theory under the daemon's
 * ratchet is that mapping pool pages consumes no new physical memory (the
 * frames are already resident), so the same total is reachable in steps.
 * PASS is all of guest RAM mapped in one process with the host alive --
 * the exact operation that used to be a bugcheck. FAIL is a refusal that
 * halving cannot move, reported with the totals; a bugcheck is its own
 * report.
 */
static int grow_test(co_manager_handle_t handle)
{
	unsigned long long base = 0, top = 0, budget, mapped = 0, pa;
	int slices = 0, ratchets = 0;

	if (!CO_OK(co_manager_window_bounds(handle, &base, &top)) || !base) {
		printf("cannot read the RAM ceiling -- old driver?\n");
		return 1;
	}

	budget = raise_ws_for(base);
	printf("grow: RAM ceiling %llu MB, initial working-set grant %llu MB\n",
	       base >> 20, budget >> 20);
	if (!budget) {
		/* Proceed regardless: with the driver's guard dispatching
		 * through ntoskrnl's handler, a mapping the host cannot make
		 * is a clean refusal now, and finding out is the point. */
		printf("grow: no grant at all; mapping anyway\n");
		budget = base;
	}

	for (pa = 0; pa < base; ) {
		co_kmap_range_t range;
		int reused = 0;

		if (mapped + (128ULL << 20) > budget && budget < base) {
			unsigned long long next = budget + (512ULL << 20);

			if (next > base)
				next = base;
			if (raise_ws_once(next)) {
				budget = next;
				ratchets++;
				printf("grow: ratcheted the grant to %llu MB"
				       " (%llu MB mapped)\n", budget >> 20,
				       mapped >> 20);
			} else {
				printf("grow: ratchet to %llu MB REFUSED (%lu)"
				       " at %llu MB mapped -- stopping short\n",
				       next >> 20,
				       (unsigned long)GetLastError(),
				       mapped >> 20);
				break;
			}
		}

		if (!CO_OK(co_manager_kmap_range(handle, pa, &range, &reused))) {
			printf("grow: KMAP_RANGE refused at pa 0x%llx"
			       " (%llu MB mapped)\n", pa, mapped >> 20);
			break;
		}
		/* Touch one byte per page so the mapping is real, not lazy. */
		{
			volatile const unsigned char* p =
				(const unsigned char*)(size_t)range.user_va;
			unsigned long long off;
			unsigned char acc = 0;

			for (off = 0; off < range.bytes; off += 4096)
				acc ^= p[off];
			(void)acc;
		}
		mapped += range.bytes;
		slices++;
		pa = range.pa + range.bytes;
	}

	printf("\ngrow: %d slices, %llu of %llu MB mapped, %d ratchet(s)\n",
	       slices, mapped >> 20, base >> 20, ratchets);
	co_manager_kunmap(handle, NULL);
	co_os_manager_close(handle);

	if (mapped + (64ULL << 20) >= base) {
		printf("grow: PASS -- all of guest RAM mapped in one process,"
		       " host alive\n");
		return 0;
	}
	printf("grow: FAIL -- stopped %llu MB short\n", (base - mapped) >> 20);
	return 1;
}

int main(int argc, char** argv)
{
	enum { MODE_CLEAN, MODE_EXIT, MODE_CRASH, MODE_STARVE } mode = MODE_CLEAN;
	co_manager_handle_t handle;
	co_manager_ioctl_kmap_t* map;
	unsigned long slice = 0;
	unsigned long long peek_va = 0;
	unsigned long peek_bytes = 128;
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
		else if (!strcmp(argv[i], "--grow")) {
			co_manager_handle_t h = co_os_manager_open();

			if (!h) {
				printf("cannot open the driver\n");
				return 1;
			}
			return grow_test(h);
		}
		else if (!strcmp(argv[i], "--slice") && i + 1 < argc)
			slice = strtoul(argv[++i], NULL, 0);
		else if (!strcmp(argv[i], "--peek-va") && i + 1 < argc)
			peek_va = _strtoui64(argv[++i], NULL, 0);
		else if (!strcmp(argv[i], "--bytes") && i + 1 < argc)
			peek_bytes = strtoul(argv[++i], NULL, 0);
		else {
			printf("usage: kmap-test [--clean|--exit|--crash|--starve]"
			       " [--slice BYTES] [--peek-va ADDRESS] [--bytes COUNT]\n");
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

	/*
	 * Read a live guest virtual address without involving either vCPU.
	 *
	 * This is intentionally part of the existing KMAP diagnostic rather
	 * than a new driver interface: Kvirt-to-phys walks the guest's current
	 * page tables in the host, KMAP_RANGE maps only the containing physical
	 * slice into this process, and neither operation asks the wedged guest
	 * to run. It is therefore useful precisely when both vCPUs are spinning
	 * and the console cannot report their interrupt state.
	 */
	if (peek_va) {
		co_kmap_range_t range;
		int reused = 0;
		unsigned long long pa = 0, off;
		const unsigned char* p;

		if (!CO_OK(co_manager_kvirt_to_phys(handle, peek_va, &pa))) {
			printf("cannot translate guest va 0x%llx\n", peek_va);
			co_os_manager_close(handle);
			return 1;
		}
		if (!CO_OK(co_manager_kmap_range(handle, pa, &range, &reused))) {
			printf("cannot map guest pa 0x%llx\n", pa);
			co_os_manager_close(handle);
			return 1;
		}

		off = pa - range.pa;
		if (off >= range.bytes) {
			printf("driver returned a range that does not contain pa 0x%llx\n",
			       pa);
			co_manager_kunmap(handle, NULL);
			co_os_manager_close(handle);
			return 1;
		}
		if ((unsigned long long)peek_bytes > range.bytes - off)
			peek_bytes = (unsigned long)(range.bytes - off);

		p = (const unsigned char*)(size_t)(range.user_va + off);
		printf("guest va 0x%llx -> pa 0x%llx, %lu byte(s)%s\n",
		       peek_va, pa, peek_bytes, reused ? " (mapping reused)" : "");
		for (i = 0; i < (int)peek_bytes; i += 16) {
			int j;

			printf("  %016llx:", peek_va + (unsigned long long)i);
			for (j = 0; j < 16 && i + j < (int)peek_bytes; j++)
				printf(" %02x", p[i + j]);
			printf("\n");
		}

		co_manager_kunmap(handle, NULL);
		co_os_manager_close(handle);
		return 0;
	}

	printf("kmap-test: mode %s, slice %lu\n",
	       mode == MODE_CLEAN ? "clean" : mode == MODE_EXIT ? "exit" :
	       mode == MODE_CRASH ? "crash" : "starve", slice);

	/*
	 * The quota, lifted before the map-everything call below asks for all
	 * of guest RAM at once. The RAM ceiling is the window-arena base --
	 * the driver states it, so this needs no guess about --mem.
	 */
	{
		unsigned long long ws_base = 0, ws_top = 0, got;

		if (!CO_OK(co_manager_window_bounds(handle, &ws_base, &ws_top)) ||
		    ws_base == 0)
			ws_base = 2ULL << 30;
		got = raise_ws_for(ws_base);
		printf("working set raised for %llu MB of mappings: %s\n",
		       ws_base >> 20, got ? "ok" : "REFUSED -- old quota applies,"
		       " expect the map to fail past it");
	}

	/*
	 * The failure path, exercised rather than assumed.
	 *
	 * MmMapLockedPagesSpecifyCache raises on a failed UserMode map. The driver
	 * now calls it through a dedicated x64 SEH frame which converts that raise
	 * to a normal KMAP refusal. This mode exhausts user VA first so the catch
	 * path is the thing being tested, not the success path.
	 *
	 * The pass condition is a clean refusal and a live host. A bugcheck
	 * here means the handler does not work; a SUCCESS here means the
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

		/* Squeeze the rest out in smaller pieces, so nothing 12 MB wide
		 * is left -- a 12 MB slice is what the driver will ask for. */
		for (;;) {
			p = VirtualAlloc(NULL, CO_KMAP_SLICE_BYTES, MEM_RESERVE,
					 PAGE_READWRITE);
			if (!p)
				break;
			held += CO_KMAP_SLICE_BYTES;
		}
		printf("no 12 MB hole remains. asking the driver to map anyway.\n");

		if (CO_OK(co_manager_kmap(handle, slice, map))) {
			printf("\nFAIL: KMAP SUCCEEDED with no address space left"
			       " -- the starvation did not take, so this run\n"
			       "      proves nothing about the probe.\n");
			co_manager_kunmap(handle, NULL);
			co_os_manager_close(handle);
			return 1;
		}

		printf("\nPASS: KMAP refused cleanly and the host is alive.\n"
		       "      The driver's SEH wrapper converted the mapping"
		       " exception into an error.\n");
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
			(const unsigned long long*)(size_t)map->range[i].user_va;
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
				(const unsigned char*)(size_t)map->range[i].user_va;
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

	case MODE_STARVE:
		/* Handled, including cleanup, before KMAP above. */
		break;
	}

	return mismatches ? 1 : 0;
}
