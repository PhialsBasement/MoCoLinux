#!/bin/sh
# Build the Linux guest test client for CoPresent.  Run on a Linux builder with
# the same baseline ABI as the guest (the accepted host and guest both use
# glibc 2.44), or run it in the guest itself.
#
# The broker this script used to build is gone. Presentation now travels as a
# command inside the client's own virgl stream, down the render node it already
# renders through, so there is no daemon, no socket, no pinned ring and no
# root-only pagemap step in the path -- see doc/building-copresent. The guest's
# half of CoPresent is now entirely inside the patched Mesa, which
# tools/build-mesa-copresent.sh builds.
#
# What remains here is the standalone producer, which is worth keeping: it
# exercises the present path without a GL application, so a presentation
# failure can be separated from a rendering one.

set -eu

HERE=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
SOURCE=$HERE/src/colinux/user/copresent
OUTPUT=${MOCO_COPRESENT_OUTPUT:-$HERE/../dist-guest}
CC=${CC:-cc}

command -v "$CC" >/dev/null 2>&1 || {
	printf 'build-copresent-guest: missing compiler: %s\n' "$CC" >&2
	exit 1
}
command -v pkg-config >/dev/null 2>&1 || {
	printf 'build-copresent-guest: missing pkg-config\n' >&2
	exit 1
}
command -v objcopy >/dev/null 2>&1 || {
	printf 'build-copresent-guest: missing objcopy\n' >&2
	exit 1
}
pkg-config --exists egl gl x11 || {
	printf 'build-copresent-guest: need EGL, OpenGL and X11 headers\n' >&2
	exit 1
}

mkdir -p "$OUTPUT"
"$CC" -O2 -Wall -Wextra -march=x86-64 -mtune=generic \
	$(pkg-config --cflags egl gl x11) \
	-o "$OUTPUT/copresent-test" \
	"$SOURCE/copresent-test.c" \
	$(pkg-config --libs egl gl x11) -lm

# This build host's CRT objects advertise its own x86-64 ISA level even when
# the source is compiled with -march=x86-64.  The conservative guest loader
# then refuses an otherwise baseline binary before main().  coxwire has the
# same requirement; the note is metadata, not code generation.
objcopy --remove-section=.note.gnu.property "$OUTPUT/copresent-test"

printf 'built guest CoPresent artifacts in %s\n' "$OUTPUT"
