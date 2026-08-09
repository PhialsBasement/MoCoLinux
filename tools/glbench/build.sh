#!/bin/sh
# Build the identical GL workload for native Windows/WGL and Linux/GLX.

set -eu

HERE=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$HERE/../.." && pwd)
OUTPUT=${MOCO_GLBENCH_OUTPUT:-$ROOT/../build/glbench}
LINUX_CC=${CC:-cc}
WINDOWS_CC=${MOCO_MINGW_CC:-x86_64-w64-mingw32-gcc}

need() { command -v "$1" >/dev/null 2>&1 || { printf 'missing %s\n' "$1" >&2; exit 1; }; }

need "$LINUX_CC"
need "$WINDOWS_CC"
need pkg-config
mkdir -p "$OUTPUT"

"$LINUX_CC" -O2 -march=x86-64 -mtune=generic -std=c99 \
	-Wall -Wextra -Werror \
	-o "$OUTPUT/moco-glbench-linux" "$HERE/glbench.c" \
	$(pkg-config --cflags --libs x11 gl) -lm

# Arch's x86-64-v3 startup objects can contribute a conservative ISA-needed
# note even when this file contains baseline-only code.  The cooperative guest
# deliberately exposes a conservative virtual CPU, so retain baseline code and
# remove that misleading loader gate (the same treatment as the guest coxwire
# helper in tools/build.sh).
objcopy --remove-section=.note.gnu.property "$OUTPUT/moco-glbench-linux"

"$WINDOWS_CC" -O2 -std=c99 -Wall -Wextra -Werror -mcrtdll=msvcrt-os \
	-o "$OUTPUT/moco-glbench.exe" "$HERE/glbench.c" \
	-lopengl32 -lgdi32 -luser32 -ld3d11 -ldxgi -ldxguid -lm

printf 'built %s\n' "$OUTPUT/moco-glbench-linux"
printf 'built %s\n' "$OUTPUT/moco-glbench.exe"
