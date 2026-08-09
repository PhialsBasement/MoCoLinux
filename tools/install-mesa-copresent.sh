#!/bin/sh
# Opt a development guest into or out of the matched Mesa DRI3/CoPresent pair
# without overwriting any package-owned library.  Normal processes then use
# direct GLX; no application wrapper, preload library or VirtualGL is involved.

set -eu

PREFIX=/usr/local/lib/moco-copresent
CONF=/etc/ld.so.conf.d/00-moco-copresent.conf
STATE=/var/lib/moco-copresent

die() { printf 'install-mesa-copresent: %s\n' "$*" >&2; exit 1; }
command -v ldconfig >/dev/null 2>&1 || die "ldconfig is required"

case ${1:-} in
enable)
	[ "$(id -u)" -eq 0 ] || die "enable must run as root inside the guest"
	INPUT=${2:-}
	if [ -d "$INPUT" ]; then
		LIBRARY=$INPUT/libGL-moco.so.1
		GALLIUM=$INPUT/libgallium-26.1.6.so
	else
		LIBRARY=$INPUT
		GALLIUM=${3:-}
	fi
	[ -f "$LIBRARY" ] && [ -f "$GALLIUM" ] ||
		die "usage: $0 enable /path/to/out-dir (or libGL-moco.so.1 libgallium-26.1.6.so)"
	mkdir -p "$PREFIX" "$STATE" "$(dirname "$CONF")"
	ldconfig -p | awk '$1 == "libGL.so.1" { print $NF; exit }' \
		> "$STATE/libGL-before"
	# R2 used a higher-looking filename (1.5.0), so ldconfig would prefer it
	# over the normal DRI GLX library even after changing libGL.so.1.  Preserve
	# that managed prototype for rollback, but take it out of the search dir.
	if [ -f "$PREFIX/libGL.so.1.5.0" ]; then
		mv -f "$PREFIX/libGL.so.1.5.0" "$STATE/libGL-r2.so.1.5.0"
	fi
	install -m 0755 "$LIBRARY" "$PREFIX/libGL.so.1.2.0"
	install -m 0755 "$GALLIUM" "$PREFIX/libgallium-26.1.6.so"
	ln -sfn libGL.so.1.2.0 "$PREFIX/libGL.so.1"
	printf '%s\n' "$PREFIX" > "$CONF"
	ldconfig
	selected=$(ldconfig -p | awk '$1 == "libGL.so.1" { print $NF; exit }')
	case $selected in
	"$PREFIX"/*) ;;
	*) die "ldconfig still selects $selected; run '$0 disable'" ;;
	esac
	missing=$(ldd "$PREFIX/libGL.so.1.2.0" | awk '/not found/ { print $1 }')
	[ -z "$missing" ] || die "runtime dependencies are missing: $missing"
	printf 'enabled: %s\n' "$selected"
	;;
disable)
	[ "$(id -u)" -eq 0 ] || die "disable must run as root inside the guest"
	rm -f "$CONF"
	ldconfig
	selected=$(ldconfig -p | awk '$1 == "libGL.so.1" { print $NF; exit }')
	printf 'disabled; libGL.so.1 now resolves to %s\n' "${selected:-nothing}"
	;;
status)
	selected=$(ldconfig -p | awk '$1 == "libGL.so.1" { print $NF; exit }')
	printf 'libGL.so.1 -> %s\n' "${selected:-nothing}"
	if [ -f "$CONF" ]; then
		printf 'CoPresent selection file: enabled\n'
	else
		printf 'CoPresent selection file: disabled\n'
	fi
	;;
*)
	die "usage: $0 {enable /path/to/out-dir|disable|status}"
	;;
esac
