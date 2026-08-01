#!/usr/bin/env python3
"""Decode a Windows x64 triage minidump and attribute its stack to linux.sys.

XP x64 has no KDNET, so this project's post-mortem evidence is a 64 KB triage
dump written by AutoReboot. windbg is not available on the Linux side, and the
parts of these dumps that matter are simple: the bugcheck code and parameters
at fixed offsets, a module list, and a captured stack region.

    decode-minidump.py DUMP [linux.sys]

Two things learned the hard way and encoded here. The stack region is not
necessarily 8-byte aligned in the file, so both alignments are tried and the
one that resolves more pointers wins -- reading only the aligned one made a
stack full of driver frames look empty. And every candidate is attributed
against the module list rather than assumed, because a bugcheck whose faulting
address is inside ntoskrnl says nothing about which driver got it there; the
caller one frame below does.

Offsets: bugcheck code at 0x38, its four parameters at 0x40, and the triage
header at 0x2000 whose fields give the call-stack region, the driver list and
the string pool.
"""

import struct
import subprocess
import sys

BUGCHECKS = {
    0x19: "BAD_POOL_HEADER",
    0x1E: "KMODE_EXCEPTION_NOT_HANDLED",
    0x3B: "SYSTEM_SERVICE_EXCEPTION",
    0x50: "PAGE_FAULT_IN_NONPAGED_AREA",
    0x7E: "SYSTEM_THREAD_EXCEPTION_NOT_HANDLED",
    0x8E: "KERNEL_MODE_EXCEPTION_NOT_HANDLED",
    0xC2: "BAD_POOL_CALLER",
    0xC5: "DRIVER_CORRUPTED_EXPOOL",
    0xD5: "DRIVER_PAGE_FAULT_IN_FREED_SPECIAL_POOL",
    0x7F: "UNEXPECTED_KERNEL_MODE_TRAP",
}

# For 0x50 the second parameter says which way the access went, and that alone
# has decided more than one diagnosis here.
ACCESS = {0: "read", 1: "write", 8: "execute"}


def load_symbols(path):
    try:
        out = subprocess.run(["x86_64-w64-mingw32-nm", "-n", path],
                             capture_output=True, text=True).stdout
    except FileNotFoundError:
        return []
    syms = []
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 3:
            try:
                syms.append((int(parts[0], 16), parts[2]))
            except ValueError:
                pass
    syms.sort()
    return syms


def symbolize(syms, addr):
    best = None
    for a, n in syms:
        if a <= addr:
            best = (a, n)
        else:
            break
    return "%s+0x%x" % (best[1], addr - best[0]) if best else "?"


def main():
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)

    dump = sys.argv[1]
    driver = sys.argv[2] if len(sys.argv) > 2 else "dist-x64/linux.sys"

    d = open(dump, "rb").read()
    if d[:8] != b"PAGEDU64":
        raise SystemExit("%s: not an x64 dump (%r)" % (dump, d[:8]))

    code = struct.unpack_from("<I", d, 0x38)[0]
    params = struct.unpack_from("<4Q", d, 0x40)

    print("%s" % dump)
    print("bugcheck 0x%X  %s" % (code, BUGCHECKS.get(code, "?")))
    for i, v in enumerate(params, 1):
        note = ""
        if code == 0x50 and i == 2:
            note = "  (%s)" % ACCESS.get(v, "?")
        print("  p%d = 0x%016x%s" % (i, v, note))

    tri = struct.unpack_from("<18I", d, 0x2000)
    cs, cslen, dl, count, pool = tri[10], tri[11], tri[12], tri[13], tri[14]
    ent = (pool - dl) // count

    mods = []
    for i in range(count):
        off = dl + i * ent
        nameptr = struct.unpack_from("<Q", d, off)[0]
        base = struct.unpack_from("<Q", d, off + 0x38)[0]
        size = struct.unpack_from("<I", d, off + 0x48)[0]
        name = "?"
        if nameptr < len(d) - 4:
            ln = struct.unpack_from("<H", d, nameptr)[0]
            name = d[nameptr + 4:nameptr + 4 + ln * 2].decode("utf-16-le", "replace")
            name = name.rstrip("\x00")
        mods.append((base, size, name))

    ours = [m for m in mods if "linux" in m[2].lower()]
    print("\n%d modules loaded" % count)
    for b, s, n in ours:
        print("  %-16s base 0x%016x  size 0x%x" % (n, b, s))
    if not ours:
        print("  linux.sys is NOT in the loaded list")

    syms = load_symbols(driver)
    lb = ours[0][0] if ours else 0
    print("  %d symbols from %s" % (len(syms), driver))

    def attribute(v):
        for b, s, n in mods:
            if b <= v < b + s:
                extra = ""
                if n.lower().startswith("linux") and syms:
                    extra = "   " + symbolize(syms, 0x10000 + (v - lb))
                return "%s+0x%x%s" % (n, v - b, extra)
        return None

    for i, v in ((1, params[0]), (3, params[2])):
        print("\np%d 0x%016x -> %s" % (i, v, attribute(v) or "not in any module"))

    print("\ncall stack region at file+0x%x, %d bytes" % (cs, cslen))
    best = None
    for align in (0, 4):
        hits = []
        for o in range(align, cslen - 8, 8):
            v = struct.unpack_from("<Q", d, cs + o)[0]
            a = attribute(v)
            if a:
                hits.append("  +0x%04x  0x%016x  %s" % (o, v, a))
        if best is None or len(hits) > len(best[1]):
            best = (align, hits)

    print("(alignment %d resolved %d pointers)" % (best[0], len(best[1])))
    for line in best[1]:
        print(line)


if __name__ == "__main__":
    main()
