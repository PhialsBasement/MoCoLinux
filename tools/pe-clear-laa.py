#!/usr/bin/env python3
"""Clear IMAGE_FILE_LARGE_ADDRESS_AWARE in a PE image, in place.

A 64-bit process without that flag is confined by Windows to a 2 GB address
space, which makes every pointer in it fit in 32 bits. That is what the
vendored slirp needs: its protocol structures are wire-format overlays, so the
queue-link fields inside them are u_int32_t, and insque_32/remque_32 store
pointers through them (see slirp/misc.c and the note in slirp_config.h). With
the flag set, mingw-w64's default image base alone puts static addresses above
4 GB and the first fragment queued corrupts memory.

binutils can do this for i386 -- ld --disable-large-address-aware -- but the
option does not exist in the x86-64 linker, which rejects it outright. Hence
this: link with --image-base 0x400000 so the image itself is low, then clear
one bit here so the heap is too.

The bit lives in COFF FileHeader.Characteristics, which is at the offset named
by e_lfanew plus 4 for the PE signature plus 18 into the header. Nothing else
in the file changes, and the checksum is not recomputed because PE checksums
are only enforced for drivers and this is a user-mode executable.
"""

import struct
import sys

IMAGE_FILE_LARGE_ADDRESS_AWARE = 0x0020


def clear_laa(path):
    with open(path, "r+b") as f:
        data = bytearray(f.read())

        if data[:2] != b"MZ":
            raise SystemExit("%s: not a PE image (no MZ)" % path)

        e_lfanew = struct.unpack_from("<I", data, 0x3C)[0]
        if data[e_lfanew:e_lfanew + 4] != b"PE\0\0":
            raise SystemExit("%s: no PE signature at e_lfanew" % path)

        # COFF header follows the 4-byte signature; Characteristics is its
        # last field, 18 bytes in.
        off = e_lfanew + 4 + 18
        (ch,) = struct.unpack_from("<H", data, off)

        if not (ch & IMAGE_FILE_LARGE_ADDRESS_AWARE):
            print("%s: already not large-address-aware (0x%04x)" % (path, ch))
            return

        new = ch & ~IMAGE_FILE_LARGE_ADDRESS_AWARE
        struct.pack_into("<H", data, off, new)
        f.seek(0)
        f.write(data)
        print("%s: characteristics 0x%04x -> 0x%04x (2 GB address space)"
              % (path, ch, new))


if __name__ == "__main__":
    if len(sys.argv) < 2:
        raise SystemExit("usage: pe-clear-laa.py FILE...")
    for name in sys.argv[1:]:
        clear_laa(name)
