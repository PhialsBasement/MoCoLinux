#!/bin/sh
# Assemble a release directory, and refuse to produce a broken one.
#
# Every release before this was assembled by hand, and the failure mode is
# always the same: a file that setup.c's payload array names is not there, the
# install gets most of the way through and dies on a machine that has just been
# wiped. So the manifest is not written here -- it is READ OUT of setup.c, and
# a missing entry stops the build rather than the install.
#
#   mkrelease.sh 0.5.0
#
# Sources are this working tree and nothing else: freshly built binaries from
# src/colinux/os/winnt/build, the signed driver and the kernel from dist-x64,
# launchers from tools/, the renderer DLLs from the cross prefix, and the root
# image from dist-x64.

set -e

VERSION=${1:?usage: mkrelease.sh VERSION}
HERE=$(cd "$(dirname "$0")/.." && pwd)
ROOT=$(cd "$HERE/.." && pwd)
BUILD=$HERE/src/colinux/os/winnt/build
DIST=$ROOT/dist-x64
PREFIX=$ROOT/download/prefix-mingw/bin
OUT=$ROOT/release/MoCoLinux-$VERSION

say() { printf '%s\n' "$*"; }
die() { printf 'mkrelease: %s\n' "$*" >&2; exit 1; }

# The version the installer draws must match the directory it ships in. Both
# are written by hand in different files, so they are compared here rather than
# trusted.
grep -q "define MOCO_VERSION \"$VERSION\"" "$HERE/installer/setup.c" ||
	die "installer/setup.c does not declare MOCO_VERSION \"$VERSION\""

rm -rf "$OUT"
mkdir -p "$OUT"

# Where each name comes from. First match wins, so a freshly built binary beats
# a staged copy of the same name.
#
# Two names are not allowed to follow that rule, and both were caught shipping
# the wrong file:
#
#   linux.sys must come from dist-x64, because the build writes it UNSIGNED and
#   tools/sign-driver.sh signs the copy there. The build-dir one loads on XP,
#   which ignores embedded signatures, and is refused outright by every later
#   Windows -- an install that fails only on the hosts most people have.
#
#   mocolinux-setup.exe must come from installer/, where it is built. A copy in
#   dist-x64 is whatever was last staged, which is how a release ships an
#   installer older than its own source.
find_src() {
	case $1 in
	linux.sys)		# the build's own signed product first, then the
				# staged copy sign-driver.sh makes by hand
				[ -f "$BUILD/linux-signed.sys" ] &&
					{ printf '%s\n' "$BUILD/linux-signed.sys"; return 0; }
				[ -f "$DIST/$1" ] && { printf '%s\n' "$DIST/$1"; return 0; }
				return 1 ;;
	mocolinux-setup.exe)	[ -f "$HERE/installer/$1" ] &&
					{ printf '%s\n' "$HERE/installer/$1"; return 0; }
				return 1 ;;
	esac
	for d in "$BUILD" "$DIST" "$HERE/tools" "$PREFIX" "$HERE" "$ROOT/download"; do
		[ -f "$d/$1" ] && { printf '%s\n' "$d/$1"; return 0; }
	done
	return 1
}

# The payload array, straight out of the installer that will read it. Anything
# between the array's opening brace and its NULL terminator that looks like a
# quoted filename is an entry; the comments in between are prose and have no
# quoted strings of that shape.
manifest=$(sed -n '/^static const char \*payload\[PAYLOAD_MAX\] = {/,/^};/p' \
		"$HERE/installer/setup.c" |
	   sed -n 's/^\t"\([^"]*\)",$/\1/p')

[ -n "$manifest" ] || die "could not read the payload array out of setup.c"

# The image and the X server are not in that array -- the first is copied to
# the Linux directory instead of beside Setup, the second is optional -- so
# they are named here, from the same constants.
image=$(sed -n 's/^#define IMAGE_BASE  "\(.*\)"$/\1/p' "$HERE/installer/setup.c")
xsrv=$(sed -n 's/^#define XSERVER_INSTALLER "\(.*\)"$/\1/p' "$HERE/installer/setup.c")

[ -n "$image" ] || die "could not read IMAGE_BASE out of setup.c"

missing=
for f in $manifest $image $xsrv mocolinux-setup.exe; do
	src=$(find_src "$f") || { missing="$missing $f"; continue; }
	cp -p "$src" "$OUT/$f"
	printf '  %-34s %s\n' "$f" "$src"
done

[ -z "$missing" ] || die "not in this tree:$missing"

# Built after the source it was built from, and after the driver code it
# installs. Both have shipped stale before.
[ "$OUT/mocolinux-setup.exe" -nt "$HERE/installer/setup.c" ] ||
	die "mocolinux-setup.exe is older than setup.c -- rebuild it first"

# The driver carries a signature, and it is a signature of THIS driver.
#
# Not "Signature verification: ok" and not osslsigncode's exit status: both
# report the CHAIN, which cannot be trusted here by construction -- the cert is
# self-signed, which is exactly what testsigning mode exists to accept. Windows
# does not check the chain with testsigning on either.
#
# What matters is the file's own integrity: the digest stored at signing time
# against the digest of the bytes as they are now. Those differ if the file was
# touched after signing, which is what a stale sign-driver.sh run looks like,
# and Windows refuses that regardless of testsigning.
sig=$(osslsigncode verify "$OUT/linux.sys" 2>&1 || true)
printf '%s\n' "$sig" | grep -q "Number of verified signatures: 1" ||
	die "linux.sys carries no signature -- run tools/sign-driver.sh"

cur=$(printf '%s\n' "$sig" | sed -n 's/^ *Current message digest *: *//p'  | head -1)
calc=$(printf '%s\n' "$sig" | sed -n 's/^ *Calculated message digest *: *//p' | head -1)
[ -n "$cur" ] && [ "$cur" = "$calc" ] ||
	die "linux.sys was modified after signing -- run tools/sign-driver.sh"

# ...and it is the driver this tree just built, not an older signed one. The
# build signs as a target now (linux-signed.sys), so this is normally already
# true; it still catches a release cut from a hand-signed copy left behind by
# an earlier build.
if [ -f "$BUILD/linux-signed.sys" ]; then
	[ "$BUILD/linux-signed.sys" -nt "$BUILD/linux.sys" ] ||
		die "linux-signed.sys is older than the driver -- rebuild"
else
	[ "$DIST/linux.sys" -nt "$BUILD/linux.sys" ] ||
		die "dist-x64/linux.sys is older than the build -- run tools/sign-driver.sh"
fi

# The Universal CRT check from installer/README, applied to every PE that
# ships. A binary linked against api-ms-win-crt-* does not start on XP at all
# and needs KB2999226 on 7 -- which is exactly the machine this exists for.
#
# No exemptions. The two that used to need one -- libgcc_s_seh-1 and
# libwinpthread-1, which the toolchain builds against the UCRT no matter what
# this tree does -- are no longer shipped at all: virglrenderer links them
# statically now, which is what this check existing is for.
for exe in "$OUT"/*.exe "$OUT"/*.dll "$OUT"/linux.sys; do
	[ -f "$exe" ] || continue
	base=$(basename "$exe")
	case $base in
	vcxsrv-*) continue ;;	# not ours; 1.14.2.1 is pinned by name
	esac
	objdump -p "$exe" 2>/dev/null | grep -qi "api-ms-win-crt" &&
		die "$base links the Universal CRT -- it will not start on XP"
done

# Nothing may depend on a DLL the release does not carry. objdump lists every
# import; anything that is not a Windows system DLL has to be beside it, and
# this is how the libgcc/libwinpthread problem would have been caught the first
# time instead of the fifth release.
for exe in "$OUT"/*.exe "$OUT"/*.dll; do
	[ -f "$exe" ] || continue
	case $(basename "$exe") in vcxsrv-*) continue ;; esac
	# Import names come back in whatever case the linker recorded --
	# IPHLPAPI.DLL and iphlpapi.dll are the same file -- so both sides of
	# this comparison are folded to lower case rather than spelled out as
	# character classes, which is how IPHLPAPI.DLL was reported missing.
	sysdlls=" kernel32.dll user32.dll gdi32.dll advapi32.dll shell32.dll \
shlwapi.dll ole32.dll oleaut32.dll ws2_32.dll msvcrt.dll ntdll.dll \
opengl32.dll glu32.dll userenv.dll version.dll comdlg32.dll comctl32.dll \
iphlpapi.dll mswsock.dll winmm.dll setupapi.dll rpcrt4.dll secur32.dll \
crypt32.dll psapi.dll dbghelp.dll ndis.sys ntoskrnl.exe hal.dll "

	objdump -p "$exe" 2>/dev/null | sed -n 's/^\tDLL Name: //p' | while read -r dep; do
		low=$(printf '%s' "$dep" | tr 'A-Z' 'a-z')
		case $sysdlls in *" $low "*) continue ;; esac
		[ -f "$OUT/$dep" ] ||
			echo "MISSING-DEP $(basename "$exe") needs $dep" >> "$OUT/.depfail"
	done
done
if [ -f "$OUT/.depfail" ]; then
	cat "$OUT/.depfail" >&2
	rm -f "$OUT/.depfail"
	die "a shipped binary depends on a DLL the release does not carry"
fi

say ""
say "MoCoLinux $VERSION assembled in $OUT"
say "  $(printf '%s\n' $manifest | wc -l) payload files, plus $image and the X server"
du -sh "$OUT" | sed 's/^/  /'
