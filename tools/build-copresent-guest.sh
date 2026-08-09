#!/bin/sh
# Build the Linux guest pieces of CoPresent.  Run on a Linux builder with the
# same baseline ABI as the guest (the accepted host and guest both use glibc
# 2.44), or run it in the guest itself.

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
pkg-config --exists libdrm egl gl x11 || {
	printf 'build-copresent-guest: need libdrm, EGL, OpenGL and X11 headers\n' >&2
	exit 1
}

mkdir -p "$OUTPUT"
"$CC" -O2 -Wall -Wextra -march=x86-64 -mtune=generic \
	$(pkg-config --cflags libdrm) \
	-o "$OUTPUT/moco-present-broker" \
	"$SOURCE/copresent-broker.c" \
	$(pkg-config --libs libdrm) -pthread
"$CC" -O2 -Wall -Wextra -march=x86-64 -mtune=generic \
	$(pkg-config --cflags egl gl x11) \
	-o "$OUTPUT/copresent-test" \
	"$SOURCE/copresent-test.c" \
	$(pkg-config --libs egl gl x11) -lm
install -m 0644 "$SOURCE/moco-present.service" "$OUTPUT/moco-present.service"

# This build host's CRT objects advertise its own x86-64 ISA level even when
# the source is compiled with -march=x86-64.  The conservative guest loader
# then refuses an otherwise baseline binary before main().  coxwire has the
# same requirement; the note is metadata, not code generation.
objcopy --remove-section=.note.gnu.property "$OUTPUT/moco-present-broker"
objcopy --remove-section=.note.gnu.property "$OUTPUT/copresent-test"

printf 'built guest CoPresent artifacts in %s\n' "$OUTPUT"
