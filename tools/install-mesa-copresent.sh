#!/bin/sh
# Opt an R2 development guest into or out of the patched Mesa libGL without
# overwriting any package-owned library.  This is intentionally explicit:
# the current GLX prototype has no fallback if the broker is absent.

set -eu

PREFIX=/usr/local/lib/moco-copresent
CONF=/etc/ld.so.conf.d/00-moco-copresent.conf
STATE=/var/lib/moco-copresent

die() { printf 'install-mesa-copresent: %s\n' "$*" >&2; exit 1; }
command -v ldconfig >/dev/null 2>&1 || die "ldconfig is required"

case ${1:-} in
enable)
	[ "$(id -u)" -eq 0 ] || die "enable must run as root inside the guest"
	LIBRARY=${2:-}
	[ -f "$LIBRARY" ] || die "usage: $0 enable /path/to/libGL-moco.so.1"
	mkdir -p "$PREFIX" "$STATE" "$(dirname "$CONF")"
	ldconfig -p | awk '$1 == "libGL.so.1" { print $NF; exit }' \
		> "$STATE/libGL-before"
	install -m 0755 "$LIBRARY" "$PREFIX/libGL.so.1.5.0"
	ln -sfn libGL.so.1.5.0 "$PREFIX/libGL.so.1"
	printf '%s\n' "$PREFIX" > "$CONF"
	ldconfig
	selected=$(ldconfig -p | awk '$1 == "libGL.so.1" { print $NF; exit }')
	case $selected in
	"$PREFIX"/*) ;;
	*) die "ldconfig still selects $selected; run '$0 disable'" ;;
	esac
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
	die "usage: $0 {enable /path/to/libGL-moco.so.1|disable|status}"
	;;
esac
