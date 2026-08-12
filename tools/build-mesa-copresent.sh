#!/bin/sh
# Build the MoCoLinux direct DRI3/CoPresent GL stack from an untouched Mesa
# 26.1.6 release tarball.  The source patch is part of this tree; the modified
# tree under download/ is never an input.

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
BUILD32=$WORK/build32
# Both ABIs, in the layout mkmanjarorootfs.sh reads them from.
OUT64=${MOCO_MESA_OUT64:-$WORK/out/lib64}
OUT32=${MOCO_MESA_OUT32:-$WORK/out/lib32}
WANT32=${MOCO_MESA_32BIT:-1}
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

# EGL, GBM and GLES are built, not disabled.
#
# They were disabled when only GLX mattered, and that quietly capped what the
# stack could run: modern toolkit and browser clients ask for EGL, and with none
# of ours present they resolved to the distribution's Mesa, probed real DRI3
# against the Windows X server, and fell back to software. Firefox with hardware
# WebRender and Steam's CEF helper are both in that group.
#
# LLVM stays disabled and llvmpipe is not built here: software Vulkan for the
# Steam client comes from the distribution's vulkan-swrast package, which the
# root filesystem installs for both ABIs. Building it here would add an LLVM
# dependency to this script for something pacman already ships.
mesa_configure() {
	# $1 = build directory, $2... = extra meson arguments
	_build=$1
	shift
	if [ -f "$_build/meson-private/coredata.dat" ]; then
		_mode=--reconfigure
	else
		_mode=
	fi
	PYTHONPATH=$VENV_SITE "$VENV/bin/meson" setup $_mode "$_build" "$SOURCE" \
		--prefix="$PREFIX" \
		-Dglx=dri \
		-Dglvnd=disabled \
		-Dgallium-drivers=virgl,softpipe \
		-Dvulkan-drivers=[] \
		-Dplatforms=x11 \
		-Degl=enabled \
		-Dgbm=enabled \
		-Dgles1=disabled \
		-Dgles2=enabled \
		-Dllvm=disabled \
		-Dgallium-va=disabled \
		-Dvideo-codecs=[] \
		-Dbuild-tests=false \
		-Dvalgrind=disabled \
		-Dlibunwind=disabled \
		"$@"
}

# Every library a client can resolve. libGL and the Gallium driver are the
# pair that does the work; the rest exist so that an application asking for
# EGL or GLES gets OUR driver rather than the distribution's.
TARGETS="src/glx/libGL.so.1.2.0
src/egl/libEGL.so.1.0.0
src/mesa/glapi/es2api/libGLESv2.so.2.0.0
src/gallium/targets/dri/libgallium-$VERSION.so
src/gbm/libgbm.so.1.0.0
src/gbm/backends/dri/dri_gbm.so"

build_abi() {
	# $1 = build dir, $2 = output dir, $3 = human label
	say "building $3 with $JOBS jobs"
	# shellcheck disable=SC2086
	PYTHONPATH=$VENV_SITE ninja -C "$1" -j "$JOBS" $TARGETS

	mkdir -p "$2"
	for t in $TARGETS; do
		install -m 0755 "$1/$t" "$2/$(basename "$t")"
		strip --strip-unneeded "$2/$(basename "$t")"
	done
}

# Venus for both ABIs: -Dvulkan-drivers=virtio builds Mesa's Venus ICD
# (libvulkan_virtio.so), which encodes Vulkan for the host's vkr. 32-bit
# matters as much as 64 -- DXVK's library is substantially 32-bit Windows
# games under Wine, and skipping it guarantees a return trip.
install_venus_icd() {
	# $1 = build dir, $2 = output dir, $3 = icd arch tag
	# The json is its own custom target, not a byproduct of the .so.
	PYTHONPATH=$VENV_SITE ninja -C "$1" -j "$JOBS" \
		src/virtio/vulkan/libvulkan_virtio.so \
		"src/virtio/vulkan/virtio_icd.$3.json"
	install -m 0755 "$1/src/virtio/vulkan/libvulkan_virtio.so" \
		"$2/libvulkan_virtio.so"
	strip --strip-unneeded "$2/libvulkan_virtio.so"
	# The loader finds the driver through this json; the path inside must
	# match where the guest installs the .so.
	install -m 0644 "$1/src/virtio/vulkan/virtio_icd.$3.json" \
		"$2/virtio_icd.$3.json"
}

say "configuring Mesa $VERSION (64-bit)"
mesa_configure "$BUILD" -Dvulkan-drivers=virtio
build_abi "$BUILD" "$OUT64" "the 64-bit stack"
say "building the 64-bit Venus ICD"
install_venus_icd "$BUILD" "$OUT64" x86_64

# The 32-bit half. Skipped rather than fatal when the build host has no
# multilib: a 64-bit-only stack is still useful, and failing the whole build
# would strand anyone who only wants GLX. The root filesystem builder warns
# loudly if the 32-bit set is missing, which is where it actually matters.
if [ "$WANT32" = 1 ]; then
	if PKG_CONFIG_LIBDIR=/usr/lib32/pkgconfig:/usr/share/pkgconfig \
	   pkg-config --exists x11 xcb libdrm 2>/dev/null &&
	   printf 'int main(void){return 0;}\n' | cc -m32 -x c - -o /dev/null 2>/dev/null
	then
		say "configuring Mesa $VERSION (32-bit)"
		# xlib-lease is a Vulkan display-lease feature needing 32-bit
		# libXrandr, which multilib installs do not always carry;
		# leases are for direct-display, which this stack never does,
		# so the Venus driver loses nothing by its absence.
		mesa_configure "$BUILD32" --cross-file="$HERE/tools/i686-cross.ini" \
			-Dxlib-lease=disabled -Dvulkan-drivers=virtio
		build_abi "$BUILD32" "$OUT32" "the 32-bit stack"
		say "building the 32-bit Venus ICD"
		install_venus_icd "$BUILD32" "$OUT32" i686
	else
		say "skipping the 32-bit stack: no multilib toolchain or i686 -dev libraries"
		say "  (Steam's client is i386 and will render in software without it)"
		OUT32=
	fi
fi

say ""
say "built the CoPresent GL stack"
sha256sum "$OUT64"/* ${OUT32:+"$OUT32"/*}
