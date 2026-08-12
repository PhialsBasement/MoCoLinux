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
	moco-boot.vbs|moco-icons.vbs|moco-term.bat|stop.bat|xstart1142.bat)
				# These are source files, not build products. comake's
				# build-directory entries are cached copies and can remain
				# valid after the tracked launcher changes.
				[ -f "$HERE/tools/$1" ] &&
					{ printf '%s\n' "$HERE/tools/$1"; return 0; }
				return 1 ;;
	libvirglrenderer-1.dll|libepoxy-0.dll)
				# Likewise, the cross prefix is authoritative for the two
				# renderer DLLs; build-directory entries are only copies.
				[ -f "$PREFIX/$1" ] &&
					{ printf '%s\n' "$PREFIX/$1"; return 0; }
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

# Do not merely find launchers with the right names: the copies in the release
# must be the tracked versions. This catches a cached comake target silently
# winning source priority and, in particular, dropping new command-line flags.
for f in moco-boot.vbs moco-icons.vbs moco-term.bat stop.bat xstart1142.bat; do
	cmp -s "$HERE/tools/$f" "$OUT/$f" ||
		die "$f is not the tracked launcher"
done

# Built after the source it was built from, and after the driver code it
# installs. Both have shipped stale before.
[ "$OUT/mocolinux-setup.exe" -nt "$HERE/installer/setup.c" ] ||
	die "mocolinux-setup.exe is older than setup.c -- rebuild it first"

# The kernel has to be one these daemons can drive.
#
# dist-x64/vmlinux is not built by this tree -- it is copied in by hand from
# whichever build/linux-* directory was last used -- so it goes stale silently
# while everything beside it is rebuilt. 0.7.0 was assembled that way, with an
# Aug 9 kernel and Aug 11 daemons, and nothing noticed until the install had
# copied 35 GB, restarted the machine, loaded the driver and then stopped at
#
#   co_colinux_timer_deadline not found -- is the kernel patched?
#
# with the guest exiting 0 and Setup able to report only that the guest had
# gone. The symbol the daemon looks up by name is the cheapest honest test of
# the pair, and it is the exact one the daemon fails on.
grep -qa co_colinux_timer_deadline "$OUT/vmlinux" ||
	die "vmlinux has no co_colinux_timer_deadline -- it predates the timer
       work and these daemons will refuse it at boot. Copy the kernel from
       the build/linux-* directory you actually built into dist-x64/vmlinux."

# The builder inside the image, replaced with this tree's copy.
#
# The image carries the script that builds the real root filesystem, and until
# now it carried whatever was baked in whenever the image was last made. That
# is the one payload nothing here checked, and it is the payload that decides
# what the installed system IS.
#
# 0.7.0 was assembled with an image holding TWO builders: /root/mk.sh from
# Aug 7 and /usr/local/bin/mkmanjarorootfs.sh from Aug 10. setup.c prefers the
# first that exists, which is /root/mk.sh -- and that one predates CoPresent,
# Venus and Vulkan entirely. An install would have run for twenty minutes and
# produced a Manjaro on stock Mesa: software GL, no Vulkan, none of the GPU
# stack this project exists for, with nothing anywhere saying so.
#
# Both paths are written, because setup.c will take either and a release
# should not depend on which. The image is the copy in $OUT, so dist-x64's
# stays as it was.
mk=$HERE/tools/mkmanjarorootfs.sh
[ -f "$mk" ] || die "tools/mkmanjarorootfs.sh is missing"

for t in debugfs e2fsck; do
	command -v $t >/dev/null 2>&1 ||
		die "$t (e2fsprogs) is needed to put the builder into $image"
done

debugfs -w -f - "$OUT/$image" >/dev/null 2>&1 <<EOF
cd /root
rm mk.sh
write $mk mk.sh
cd /usr/local/bin
rm mkmanjarorootfs.sh
write $mk mkmanjarorootfs.sh
EOF

# debugfs unlinks without tidying up after itself; 0 is clean and 1 is
# "errors corrected", which is the normal outcome here. 4 and above is damage.
e2fsck -fy "$OUT/$image" >/dev/null 2>&1
[ $? -lt 4 ] || die "$image did not survive having the builder written into it"

# Read both back and compare, rather than trusting that the write landed --
# debugfs reports a failed write on stdout and still exits 0.
check=$(mktemp -d)
for p in /root/mk.sh /usr/local/bin/mkmanjarorootfs.sh; do
	debugfs -R "dump $p $check/got" "$OUT/$image" >/dev/null 2>&1
	cmp -s "$check/got" "$mk" || {
		rm -rf "$check"
		die "$p in $image is not tools/mkmanjarorootfs.sh"
	}
done
rm -rf "$check"
say "  builder written into $image at both paths setup.c looks in"

# The X server with a monitor in it, built here rather than carried.
#
# Stock VcXsrv 1.14 answers RandR with no outputs at all, and a client that
# asks about monitors before opening a window does not degrade -- Steam's
# client refuses to start. Every install before this one shipped that way: the
# NSIS installer laid down a stock server and nothing patched it, so the fault
# reached every user who installed the release rather than only the box it was
# developed on.
#
# Built from the stock binary at assembly time so it cannot drift from the
# patcher, and the patcher verifies six byte signatures before it writes -- a
# different VcXsrv build fails here rather than shipping a corrupt server.
patched=$(sed -n 's/^#define XSERVER_PATCHED[[:space:]]*"\(.*\)"$/\1/p' \
		"$HERE/installer/setup.c")
[ -n "$patched" ] || die "could not read XSERVER_PATCHED out of setup.c"

stock=$DIST/vcxsrv-stock.exe
[ -f "$stock" ] ||
	die "dist-x64/vcxsrv-stock.exe is missing -- it is the unmodified
       vcxsrv.exe from the shipped installer, and $patched is built from it"

command -v python3 >/dev/null 2>&1 ||
	die "python3 is needed to build $patched"

"$HERE/tools/vcxsrv-fakemonitor/patch-vcxsrv.py" "$stock" "$OUT/$patched" \
	>/dev/null || die "could not build $patched from vcxsrv-stock.exe"

# The patch lands in a section of its own; if it is not there, nothing was done.
objdump -h "$OUT/$patched" 2>/dev/null | grep -q '\.moco' ||
	die "$patched has no .moco section -- the monitor patch did not apply"

say "  $patched built from the stock server"

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
