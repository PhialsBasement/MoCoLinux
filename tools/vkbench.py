#!/usr/bin/env python3
"""
vkbench -- the speed ledger's instrument. Runs IN THE GUEST, as root.

One command, every number the Vulkan stack's health is judged by. Run it
after any change that touches a data or command path, and append the row it
prints to tools/bench-ledger.txt with the commit hash. A milestone without a
number is how the GL path died: regressions and serialization points landed
invisibly across mountains of code and can no longer be localized. Numbers
first, always.

Benches (each prints MiB/s; missing prerequisites report SKIP, not silence):
  anon-write/read    memcpy against anonymous RAM -- the ceiling
  blob-write/read    guest blob (renderD128 CREATE_BLOB GUEST) via mmap
  transfer-to-host   classic staged copy: 16 MB texture x16, TRANSFER+WAIT
  window-write/read  host memory window via /dev/mem, if one is up
                     (pass --window PA when cogpu-daemon --kwindow-test is
                      waiting; skipped otherwise)
Venus rows arrive with the Venus milestone: vkMapMemory bandwidth, submit
round-trip, fence latency. This file is where they get added.
"""
import argparse, array, fcntl, mmap, os, struct, sys, time

MB = 1 << 20
TOTAL = 256 * MB


def bw(fn, total=TOTAL):
    t0 = time.perf_counter()
    fn()
    return total / (time.perf_counter() - t0) / MB


def row(name, mibs):
    print("%-18s %9.1f MiB/s" % (name, mibs))


def skip(name, why):
    print("%-18s      SKIP (%s)" % (name, why))


def bench_pair(name, m, size, payload, rounds):
    """One memmove per pass in each direction -- symmetric, no hidden
    second copy. Numbers are DRAM-scale (the payload defeats caching);
    2 MB working sets measured cache, which is how 15 GiB/s got into an
    early ledger row."""
    import ctypes
    sink = bytearray(size)
    src = (ctypes.c_char * size).from_buffer_copy(payload)
    dst = (ctypes.c_char * size).from_buffer(sink)
    win = (ctypes.c_char * size).from_buffer(m)

    def wr():
        for _ in range(rounds):
            ctypes.memmove(win, src, size)

    def rd():
        for _ in range(rounds):
            ctypes.memmove(dst, win, size)

    row(name + "-write", bw(wr, rounds * size))
    row(name + "-read", bw(rd, rounds * size))
    del win  # release the exported buffer so m.close() can succeed


def drm_ioctl(fd, nr, fmt, *vals):
    a = array.array("B", struct.pack(fmt, *vals))
    fcntl.ioctl(fd, (3 << 30) | (len(a) << 16) | (ord("d") << 8) | (0x40 + nr),
                a, True)
    return a


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--window", type=lambda s: int(s, 0), default=0,
                    help="guest pa of a live kwindow (host must be waiting)")
    args = ap.parse_args()

    size = 16 * MB
    rounds = TOTAL // size
    payload = os.urandom(size)

    m = mmap.mmap(-1, size)
    bench_pair("anon", m, size, payload, rounds)
    m.close()

    try:
        fd = os.open("/dev/dri/renderD128", os.O_RDWR)
    except OSError as e:
        skip("blob", str(e))
        skip("transfer", "no renderD128")
        fd = -1

    if fd >= 0:
        try:
            r = drm_ioctl(fd, 0x0A, "<IIII QQ QQ",
                          1, 1, 0, 0, size, 0, 0, 0)  # GUEST | MAPPABLE
            bo = struct.unpack_from("<I", r.tobytes(), 8)[0]
            mo = drm_ioctl(fd, 0x01, "<QII", 0, bo, 0)
            off = struct.unpack_from("<Q", mo)[0]
            m = mmap.mmap(fd, size, mmap.MAP_SHARED,
                          mmap.PROT_READ | mmap.PROT_WRITE, offset=off)
            m[0:size] = payload  # fault everything in before timing
            bench_pair("blob", m, size, payload, rounds)
            m.close()
        except OSError as e:
            skip("blob", "errno %s" % e)

        try:
            r = drm_ioctl(fd, 0x04, "<14I", 2, 1, 8, 2048, 2048, 1, 1,
                          0, 0, 0, 0, 0, size, 0)
            bo = struct.unpack_from("<I", r.tobytes(), 40)[0]
            mo = drm_ioctl(fd, 0x01, "<QII", 0, bo, 0)
            off = struct.unpack_from("<Q", mo)[0]
            m = mmap.mmap(fd, size, mmap.MAP_SHARED,
                          mmap.PROT_READ | mmap.PROT_WRITE, offset=off)
            m[0:size] = payload

            def xfer():
                for _ in range(rounds):
                    drm_ioctl(fd, 0x07, "<11I", bo, 0, 0, 0, 2048, 2048, 1,
                              0, 0, 0, 0)
                    drm_ioctl(fd, 0x08, "<II", bo, 0)
            row("transfer-to-host", bw(xfer))
            m.close()
        except OSError as e:
            skip("transfer", "errno %s" % e)
        os.close(fd)

    if args.window:
        try:
            fd = os.open("/dev/mem", os.O_RDWR | os.O_SYNC)
            wsz = 2 * MB
            m = mmap.mmap(fd, wsz, mmap.MAP_SHARED,
                          mmap.PROT_READ | mmap.PROT_WRITE,
                          offset=args.window)
            bench_pair("window", m, wsz, payload[:wsz], TOTAL // wsz)
            m.close()
            os.close(fd)
        except OSError as e:
            skip("window", str(e))
    else:
        skip("window", "no --window PA given")

    print("append to tools/bench-ledger.txt with: date, commit, box, rows")
    return 0


if __name__ == "__main__":
    sys.exit(main())
