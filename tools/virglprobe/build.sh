#!/bin/sh
# Build virglprobe (R2) against the cross-built virglrenderer.
#
# The dependencies are built once by download/ -- see doc notes for R2:
#   libepoxy 1.5.10 and virglrenderer 1.3.0, both meson cross builds with
#   download/mingw64-cross.ini, installed into download/prefix-mingw.
#
# -mcrtdll=msvcrt-os for the same reason as every other binary here: the
# Universal CRT does not exist on XP and is absent from a stock Windows 7.

set -e

PREFIX=/mnt/big-bricks/RProject/MoCoLinux/download/prefix-mingw
OUT=/mnt/big-bricks/RProject/MoCoLinux/dist-x64
HERE=$(dirname "$0")

x86_64-w64-mingw32-gcc \
	"$HERE/virglprobe.c" "$HERE/wgl_winsys.c" \
	-o "$OUT/virglprobe.exe" \
	-O2 -mcrtdll=msvcrt-os \
	-I"$PREFIX/include" \
	-L"$PREFIX/lib" \
	-lvirglrenderer -lepoxy -lopengl32 -lgdi32 -luser32

echo "built $OUT/virglprobe.exe"
x86_64-w64-mingw32-objdump -p "$OUT/virglprobe.exe" | grep -i "dll name"
