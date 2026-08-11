#!/usr/bin/env python3
"""
Patch VcXsrv 1.14.2.1's vcxsrv.exe so it reports one monitor.

    ./patch-vcxsrv.py vcxsrv.exe vcxsrv-patched.exe

Why this exists, and why it is a patch rather than a build: see fakemonitor.S,
which is the code this installs. The short version is that the server already
contains the routine that manufactures a CRTC and a connected output -- it is
just never reached, because XWin's rrGetInfo hook forgets to register a screen
size. This replaces that hook with one that registers it.

The whole edit is two things:

  * a new section holding the replacement hook, appended to the file;
  * one displacement inside winRandRInit(), repointed from XWin's hook to ours.

Nothing else in the image is touched, and the server is byte-identical
everywhere else.

The build is not reproducible from source -- VcXsrv 1.14.2.1 needs a 2013 MSVC
toolchain we do not have, and the installer is a pinned input -- so every
address below was read out of the shipped binary with the 1.14.2.1 source as
the map. That makes them assumptions about someone else's build, so each one is
verified against the bytes actually present before anything is written: a
different build fails loudly here instead of producing a server that crashes on
the first client.
"""

import os
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ASM = os.path.join(HERE, "fakemonitor.S")

IMAGE_BASE = 0x140000000

# Addresses in the shipped vcxsrv.exe (VcXsrv 1.14.2.1, x64).
WIN_RANDR_GETINFO = 0x140057D50  # hw/xwin/winrandr.c: winRandRGetInfo()
LEA_SITE = 0x140058043  # winRandRInit(): lea of the above into rrGetInfo
RR_PRIV_KEY_SIZE = 0x140354C04  # rrPrivKey.size, the dixLookupPrivate branch
RR_PRIV_KEY_OFFSET = 0x140354C00  # rrPrivKey.offset
MALLOC_IAT = 0x14028D660  # MSVCR100.dll!malloc thunk

# Each entry is a claim this patch depends on, and the bytes that prove it.
# They are deliberately anchored on the code that reads the struct fields
# fakemonitor.S writes, so a build with a different layout cannot match.
SIGNATURES = [
    (
        WIN_RANDR_GETINFO,
        "40534883ec3041ba00100000bb010000",
        "winRandRGetInfo(): sets rotations and the 4096x4096 size range",
    ),
    (
        LEA_SITE,
        "488d0d06fdffff",
        "winRandRInit(): lea winRandRGetInfo(%rip), %rcx",
    ),
    (
        0x1400A4EDD,
        "83bef000000000",
        "RRGetInfo(): cmpl $0, 0xf0(%rsi) -- nSizes lives at 0xf0",
    ),
    (
        0x1400A6F0A,
        "833df3dc2a0000",
        "RRCrtcCreate(): the dixLookupPrivate branch on rrPrivKey.size",
    ),
    (
        0x140057E20,
        "66896f0c6689770e664489671066894712",
        "winDoRandRScreenSetSize(): ScreenRec width/height/mm at 0xc..0x12",
    ),
    (
        0x1400A3010,
        "48894c2408555657",
        "RRScanOldConfig(): the routine that builds the CRTC and output",
    ),
]


class PE:
    def __init__(self, data):
        self.d = bytearray(data)
        self.pe = struct.unpack_from("<I", self.d, 0x3C)[0]
        if self.d[self.pe : self.pe + 4] != b"PE\0\0":
            raise SystemExit("not a PE file")
        self.nsec = struct.unpack_from("<H", self.d, self.pe + 6)[0]
        self.opt = self.pe + 24
        szopt = struct.unpack_from("<H", self.d, self.pe + 20)[0]
        self.sect = self.opt + szopt
        self.sect_align = struct.unpack_from("<I", self.d, self.opt + 32)[0]
        self.file_align = struct.unpack_from("<I", self.d, self.opt + 36)[0]
        self.base = struct.unpack_from("<Q", self.d, self.opt + 24)[0]
        self.size_of_image = struct.unpack_from("<I", self.d, self.opt + 56)[0]

    def sections(self):
        for i in range(self.nsec):
            o = self.sect + i * 40
            vsize, rva, rsize, roff = struct.unpack_from("<IIII", self.d, o + 8)
            yield o, vsize, rva, rsize, roff

    def va_to_off(self, va):
        rva = va - self.base
        for _, vsize, srva, rsize, roff in self.sections():
            if srva <= rva < srva + max(vsize, rsize):
                return roff + (rva - srva)
        raise SystemExit("address %#x is not in any section" % va)


def align(n, a):
    return (n + a - 1) // a * a


def assemble(cave_va):
    """Assemble fakemonitor.S linked at cave_va, and return the raw bytes.

    Linking at the final address is what makes the result position-independent:
    every reference in the source is RIP-relative or a relative call, so the
    linker resolves them to displacements that stay correct wherever Windows
    decides to load the image.
    """
    tmp = tempfile.mkdtemp(prefix="fakemonitor.")
    obj = os.path.join(tmp, "fakemonitor.o")
    elf = os.path.join(tmp, "fakemonitor.elf")
    binf = os.path.join(tmp, "fakemonitor.bin")

    subprocess.run(["gcc", "-c", "-o", obj, ASM], check=True)
    subprocess.run(
        [
            "ld",
            "-o",
            elf,
            obj,
            "--entry=moco_randr_getinfo",
            "-Ttext=%#x" % cave_va,
            "--defsym",
            "winRandRGetInfo=%#x" % WIN_RANDR_GETINFO,
            "--defsym",
            "rrPrivKeySize=%#x" % RR_PRIV_KEY_SIZE,
            "--defsym",
            "rrPrivKeyOffset=%#x" % RR_PRIV_KEY_OFFSET,
            "--defsym",
            "malloc_iat=%#x" % MALLOC_IAT,
        ],
        check=True,
    )
    # -j .text, because the toolchain also emits a .note.gnu.property section
    # at the next page and a plain conversion would pad the gap between them
    # into the output.
    subprocess.run(["objcopy", "-O", "binary", "-j", ".text", elf, binf],
                   check=True)
    with open(binf, "rb") as f:
        return f.read()


def main():
    if len(sys.argv) != 3:
        raise SystemExit("usage: patch-vcxsrv.py <vcxsrv.exe> <output.exe>")
    src, dst = sys.argv[1], sys.argv[2]

    with open(src, "rb") as f:
        pe = PE(f.read())

    if pe.base != IMAGE_BASE:
        raise SystemExit("unexpected image base %#x" % pe.base)

    print("verifying this is the build these addresses were read from")
    for va, expect, what in SIGNATURES:
        off = pe.va_to_off(va)
        got = bytes(pe.d[off : off + len(expect) // 2]).hex()
        if got != expect:
            raise SystemExit(
                "  MISMATCH at %#x\n"
                "    expected %s\n"
                "    found    %s\n"
                "  %s\n"
                "This is not the VcXsrv 1.14.2.1 build this patch was written\n"
                "for. Nothing has been written." % (va, expect, got, what)
            )
        print("  ok  %#x  %s" % (va, what))

    # winRandRInit's lea is the one instruction we repoint. Its displacement is
    # relative to the end of the instruction, which is 7 bytes long.
    lea_off = pe.va_to_off(LEA_SITE)
    disp = struct.unpack_from("<i", pe.d, lea_off + 3)[0]
    target = LEA_SITE + 7 + disp
    if target != WIN_RANDR_GETINFO:
        raise SystemExit(
            "the rrGetInfo hook already points at %#x, not winRandRGetInfo -- "
            "this file looks patched already" % target
        )

    cave_rva = align(pe.size_of_image, pe.sect_align)
    cave_va = pe.base + cave_rva
    print("assembling the replacement hook, linked at %#x" % cave_va)
    blob = assemble(cave_va)
    print("  %d bytes" % len(blob))

    # A new section for it. There is room for another header: the table ends
    # well before the first section's raw data.
    hdr = pe.sect + pe.nsec * 40
    if hdr + 40 > min(r for _, _, _, _, r in pe.sections()):
        raise SystemExit("no room in the section table for another entry")
    if any(pe.d[hdr : hdr + 40]):
        raise SystemExit("section table slot at %#x is not free" % hdr)

    raw_off = align(len(pe.d), pe.file_align)
    raw_size = align(len(blob), pe.file_align)
    pe.d.extend(b"\0" * (raw_off - len(pe.d)))
    pe.d.extend(blob)
    pe.d.extend(b"\0" * (raw_size - len(blob)))

    struct.pack_into(
        "<8sIIIIIIHHI",
        pe.d,
        hdr,
        b".moco",
        len(blob),  # VirtualSize
        cave_rva,
        raw_size,
        raw_off,
        0,
        0,
        0,
        0,
        0x60000020,  # CODE | EXECUTE | READ
    )
    struct.pack_into("<H", pe.d, pe.pe + 6, pe.nsec + 1)
    struct.pack_into(
        "<I", pe.d, pe.opt + 56, cave_rva + align(len(blob), pe.sect_align)
    )

    # And the one-instruction redirect.
    struct.pack_into("<i", pe.d, lea_off + 3, cave_va - (LEA_SITE + 7))
    print("repointed the rrGetInfo hook at %#x from %#x to %#x"
          % (LEA_SITE, WIN_RANDR_GETINFO, cave_va))

    with open(dst, "wb") as f:
        f.write(pe.d)
    print("wrote %s (%d bytes)" % (dst, len(pe.d)))


if __name__ == "__main__":
    main()
