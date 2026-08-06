#!/bin/sh
# Build the Windows side and leave something you can actually run.
#
#   tools/build.sh                 build, sign, stage into dist-x64
#   tools/build.sh --release 0.5.0 ...and assemble release/MoCoLinux-0.5.0
#
# What this exists for: `make.py colinux` produces binaries in
# src/colinux/os/winnt/build and stops there. What it leaves behind is not a
# working install and never was --
#
#   * linux.sys is UNSIGNED. It loads on XP, which ignores embedded
#     signatures, and is refused by every later Windows. The signed copy is a
#     separate file made by a separate script, and a release has shipped the
#     wrong one.
#   * the renderer DLLs cogpu-daemon needs are in the cross prefix, not beside
#     it, so the daemon it just built cannot start.
#   * the launchers and the boot scripts live in tools/, so a staging
#     directory assembled by hand is missing whichever one was forgotten.
#
# Every one of those has cost a debugging session on the real machine. So the
# build ends with a directory whose contents are checked, rather than a set of
# files someone has to remember to collect.
#
# What it does NOT build, and says so rather than pretending: vmlinux (built
# from the patched 7.1.5 tree) and root-arch.img (built by
# tools/mkmanjarorootfs.sh). Both are large, slow, and change rarely. It
# reports whether the staged copies are present.

set -e

HERE=$(cd "$(dirname "$0")/.." && pwd)
ROOT=$(cd "$HERE/.." && pwd)
BUILD=$HERE/src/colinux/os/winnt/build
DIST=$ROOT/dist-x64
PREFIX=${COLINUX_VIRGL_PREFIX:-$ROOT/download/prefix-mingw}

release=

while [ $# -gt 0 ]; do
	case $1 in
	--release) release=${2:?--release needs a version}; shift 2 ;;
	*) echo "usage: build.sh [--release VERSION]" >&2; exit 2 ;;
	esac
done

say() { printf '%s\n' "$*"; }
die() { printf 'build: %s\n' "$*" >&2; exit 1; }

# The 2.6.33 tree, for headers only -- the passage-page ABI is a header inside
# the guest kernel tree. Located rather than demanded, the same way the virgl
# prefix now is; the environment still wins if it is set.
KSRC=${COLINUX_TARGET_KERNEL_SOURCE:-$ROOT/build/linux-2.6.33.7-source}
KBUILD=${COLINUX_TARGET_KERNEL_BUILD:-$ROOT/build/linux-2.6.33.7-build}
[ -d "$KSRC" ] || die "no 2.6.33 header tree at $KSRC (set COLINUX_TARGET_KERNEL_SOURCE)"

say "building"
( cd "$HERE/src" && \
  COLINUX_ARCH=x86_64 \
  COLINUX_HOST_OS=winnt \
  COLINUX_TARGET_KERNEL_SOURCE="$KSRC" \
  COLINUX_TARGET_KERNEL_BUILD="$KBUILD" \
  python3 ../bin/make.py colinux ) > "$HERE/.build.log" 2>&1 ||
	{ tail -25 "$HERE/.build.log" >&2; die "build failed (full log in .build.log)"; }

# The installer, from installer/README's own recipe rather than from memory.
#
# -mcrtdll=msvcrt-os is not optional: without it mingw links the Universal CRT,
# which XP does not have at all and a stock Windows 7 does not either, and the
# exe then fails to start on exactly the machines it exists for. mkrelease.sh
# checks the result, but building it correctly here means the check never has
# anything to catch.
say "building the installer"
( cd "$HERE/installer" && \
  x86_64-w64-mingw32-windres setup.rc -O coff -o setup.res && \
  x86_64-w64-mingw32-gcc setup.c setup.res -o mocolinux-setup.exe -O2 -mwindows \
	-mcrtdll=msvcrt-os -lshlwapi -lole32 -lws2_32 -ladvapi32 \
) >> "$HERE/.build.log" 2>&1 ||
	{ tail -25 "$HERE/.build.log" >&2; die "installer build failed"; }

say "signing the driver"
"$HERE/tools/sign-driver.sh" >/dev/null ||
	die "could not sign linux.sys"

say "staging into dist-x64"
mkdir -p "$DIST"

# The binaries this build produced. linux.sys is deliberately absent: it is
# staged by sign-driver.sh above, signed, and copying the unsigned one over it
# here would undo that -- which is exactly the mistake this script exists to
# make impossible.
for f in colinux-daemon.exe colinux-slirp-net-daemon.exe cogpu-daemon.exe \
	 colinux-console-nt.exe colinux-debug-daemon.exe \
	 colinux-net-daemon.exe colinux-ndis-net-daemon.exe \
	 colinux-serial-daemon.exe; do
	[ -f "$BUILD/$f" ] && cp -p "$BUILD/$f" "$DIST/$f"
done

# The renderer, from the cross prefix. cogpu-daemon does not start without
# these and the loader says nothing useful about why.
for f in libvirglrenderer-1.dll libepoxy-0.dll; do
	if [ -f "$PREFIX/bin/$f" ]; then
		cp -p "$PREFIX/bin/$f" "$DIST/$f"
	else
		say "  WARNING: $f not in $PREFIX/bin -- the GPU daemon will not start"
	fi
done

# The launchers and helpers, from the tree rather than from whatever was last
# edited in the staging directory.
for f in moco-boot.vbs moco-icons.vbs moco-term.bat stop.bat \
	 xstart1142.bat setup-net.bat coterm.py; do
	[ -f "$HERE/tools/$f" ] && cp -p "$HERE/tools/$f" "$DIST/$f"
done

# The guest-side X shim, built for the target rather than for this machine.
# -march=x86-64 and the note stripped: a build host newer than the Ivy Bridge
# produces a binary that dies with "CPU ISA level is lower than required"
# before main().
if command -v cc >/dev/null 2>&1; then
	cc -O2 -march=x86-64 -mtune=generic -o "$DIST/coxwire" \
	   "$HERE/src/colinux/user/coxwire/coxwire-guest.c" -lpthread 2>/dev/null &&
		objcopy --remove-section=.note.gnu.property "$DIST/coxwire" 2>/dev/null || true
fi

# What a working install needs, checked rather than assumed. The two this
# script does not build are reported by name so their absence is a decision
# rather than a surprise at install time.
missing=
for f in linux.sys colinux-daemon.exe colinux-slirp-net-daemon.exe \
	 cogpu-daemon.exe libvirglrenderer-1.dll libepoxy-0.dll \
	 moco-boot.vbs moco-icons.vbs moco-term.bat stop.bat xstart1142.bat; do
	[ -f "$DIST/$f" ] || missing="$missing $f"
done
[ -z "$missing" ] || die "staging is incomplete:$missing"

say ""
say "staged in $DIST"
for f in vmlinux root-arch.img; do
	if [ -f "$DIST/$f" ]; then
		say "  $f present (not built here; $(du -h "$DIST/$f" | cut -f1))"
	else
		say "  $f MISSING -- build it before making a release"
	fi
done

if [ -n "$release" ]; then
	say ""
	"$HERE/tools/mkrelease.sh" "$release"
fi
