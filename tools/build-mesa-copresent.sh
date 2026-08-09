#!/bin/sh
# Build the MoCoLinux direct-present libGL from an untouched Mesa 26.1.6
# release tarball.  The source patch is part of this tree; the modified tree
# under download/ is never an input.

set -eu

HERE=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
OUTER=$(CDPATH= cd -- "$HERE/.." && pwd)
VERSION=26.1.6
TARBALL=${MOCO_MESA_TARBALL:-$OUTER/download/mesa-$VERSION.tar.xz}
PATCH_FILE=$HERE/patch/mesa-$VERSION/moco-copresent.diff
WORK=${MOCO_MESA_WORK:-$OUTER/build/mesa-copresent-$VERSION}
SOURCE=$WORK/mesa-$VERSION
BUILD=$WORK/build
VENV=${MOCO_MESA_VENV:-$WORK/venv}
OUTPUT=${MOCO_MESA_OUTPUT:-$WORK/out/libGL-moco.so.1}
PREFIX=${MOCO_MESA_PREFIX:-/usr}
EXPECTED_TARBALL_SHA256=5296b88a0f1e012e2cb9ada150a2bbadf728ca81e5a4fb2ab43c83a4d2158606

say() { printf '%s\n' "$*"; }
die() { printf 'build-mesa-copresent: %s\n' "$*" >&2; exit 1; }
need() { command -v "$1" >/dev/null 2>&1 || die "missing required command: $1"; }

for command in cc c++ pkg-config python3 ninja patch sha256sum tar strip; do
	need "$command"
done
[ -f "$TARBALL" ] || die "Mesa tarball not found: $TARBALL"
[ -f "$PATCH_FILE" ] || die "Mesa patch not found: $PATCH_FILE"

actual_tarball_sha256=$(sha256sum "$TARBALL" | awk '{print $1}')
[ "$actual_tarball_sha256" = "$EXPECTED_TARBALL_SHA256" ] ||
	die "Mesa tarball checksum mismatch: $actual_tarball_sha256"

mkdir -p "$WORK"
if [ ! -d "$SOURCE" ]; then
	say "extracting Mesa $VERSION"
	tar -xf "$TARBALL" -C "$WORK"
fi

patch_sha256=$(sha256sum "$PATCH_FILE" | awk '{print $1}')
marker=$SOURCE/.moco-copresent-patch-sha256
if [ -f "$marker" ]; then
	read -r applied_sha256 < "$marker"
	[ "$applied_sha256" = "$patch_sha256" ] ||
		die "source has a different MoCo patch; use a fresh MOCO_MESA_WORK"
else
	say "checking and applying moco-copresent.diff"
	patch --dry-run --fuzz=0 -p1 -d "$SOURCE" < "$PATCH_FILE" >/dev/null
	patch --fuzz=0 -p1 -d "$SOURCE" < "$PATCH_FILE"
	printf '%s\n' "$patch_sha256" > "$marker"
fi

if [ ! -x "$VENV/bin/python" ]; then
	say "creating Python build environment"
	python3 -m venv "$VENV"
fi

# These are the exact Python-side versions used for the hardware acceptance
# build.  Mesa's generated sources run under Python too, hence PYTHONPATH
# below rather than relying only on the meson entry point's shebang.
say "installing pinned Meson/Python build dependencies"
"$VENV/bin/python" -m pip install --disable-pip-version-check --no-input \
	meson==1.11.2 Mako==1.4.1 MarkupSafe==3.0.3 \
	PyYAML==6.0.3 packaging==26.3
VENV_SITE=$("$VENV/bin/python" -c 'import site; print(site.getsitepackages()[0])')

if [ -n "${MOCO_BUILD_JOBS:-}" ]; then
	JOBS=$MOCO_BUILD_JOBS
else
	JOBS=$(getconf _NPROCESSORS_ONLN 2>/dev/null || printf '1\n')
fi
case $JOBS in
	''|*[!0-9]*|0) die "MOCO_BUILD_JOBS must be a positive integer" ;;
esac

say "configuring Mesa $VERSION"
if [ -f "$BUILD/meson-private/coredata.dat" ]; then
	SETUP_MODE=--wipe
else
	SETUP_MODE=
fi
PYTHONPATH=$VENV_SITE "$VENV/bin/meson" setup $SETUP_MODE "$BUILD" "$SOURCE" \
	--prefix="$PREFIX" \
	-Dglx=xlib \
	-Dgallium-drivers=virgl,softpipe \
	-Dvulkan-drivers=[] \
	-Dplatforms=x11 \
	-Degl=disabled \
	-Dgbm=disabled \
	-Dgles1=disabled \
	-Dgles2=disabled \
	-Dllvm=disabled \
	-Dgallium-va=disabled \
	-Dvideo-codecs=[] \
	-Dbuild-tests=false \
	-Dvalgrind=disabled \
	-Dlibunwind=disabled

TARGET=src/gallium/targets/libgl-xlib/libGL.so.1.5.0
say "building $TARGET with $JOBS jobs"
PYTHONPATH=$VENV_SITE ninja -C "$BUILD" -j "$JOBS" "$TARGET"

mkdir -p "$(dirname "$OUTPUT")"
install -m 0755 "$BUILD/$TARGET" "$OUTPUT"
strip --strip-unneeded "$OUTPUT"

say ""
say "built $OUTPUT"
sha256sum "$OUTPUT"
