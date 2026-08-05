#!/bin/sh
# Sign linux.sys for Windows 7 x64 test-signing mode.
#
# NT 6.x x64 refuses to load an unsigned kernel driver -- not a bugcheck, just
# ERROR_INVALID_IMAGE_HASH at sc start. With `bcdedit /set testsigning on` the
# chain is not validated, so any well-formed Authenticode signature loads; this
# signs with the self-made cert in $KEYS. XP x64 ignores embedded signatures
# entirely, so one signed binary serves both hosts.
#
# Everything is SHA-1: a Windows 7 install without KB3033929 cannot verify a
# SHA-256 Authenticode signature, and a freshly installed dual-boot box will
# not have that update. Security is not the point of a test cert.
#
#   sign-driver.sh [file.sys ...]     default: dist-x64/linux.sys
#
# The cert pair is generated once by hand (see doc/porting-x86_64) and lives
# outside the repository:
KEYS=/mnt/big-bricks/RProject/MoCoLinux/keys
CERT=$KEYS/moco-test.crt
KEY=$KEYS/moco-test.key

set -e

command -v osslsigncode >/dev/null || {
	echo "osslsigncode not installed (pacman -S osslsigncode)" >&2
	exit 1
}
[ -f "$CERT" ] && [ -f "$KEY" ] || {
	echo "missing $CERT / $KEY -- generate with:" >&2
	echo "  openssl req -x509 -newkey rsa:2048 -keyout moco-test.key \\" >&2
	echo "    -out moco-test.crt -days 3650 -nodes -sha1 \\" >&2
	echo "    -subj '/CN=MoCoLinux Test Signing' -addext extendedKeyUsage=codeSigning" >&2
	exit 1
}

# Take the driver the build just produced, not whatever is sitting in dist-x64.
#
# These are two different files and nothing connected them. The build writes
# src/colinux/os/winnt/build/linux.sys -- a symlink into a content-hashed
# .comake.build cache -- while this script signed dist-x64/linux.sys, which no
# step ever refreshed. So make.py said "Targets rebuilt", signing said "signed",
# the copy to the target machine said "1 file(s) copied", and the md5 matched on
# both ends, because it was the same stale binary at every step.
#
# That cost an entire session. Five driver builds were deployed and four
# machine reboots performed against a linux.sys that contained none of the
# changes, while the unchanged behaviour was read as evidence and three further
# "fixes" were built on top of it. The one that mattered -- a 32-bit truncation
# of user addresses in co_manager_kmap -- had been correct from the first
# attempt and simply never reached the machine.
#
# Copying here, rather than trusting the caller, because the failure is silent:
# a stale signed driver is indistinguishable from a fresh one until the machine
# behaves oddly hours later.
BUILT=/mnt/big-bricks/RProject/MoCoLinux/mocolinux/src/colinux/os/winnt/build/linux.sys
DIST=/mnt/big-bricks/RProject/MoCoLinux/dist-x64/linux.sys

if [ $# -eq 0 ] && [ -e "$BUILT" ]; then
	cp -L "$BUILT" "$DIST" || exit 1
	echo "  dist-x64/linux.sys <- fresh build ($(md5sum "$DIST" | cut -c1-12))"
fi

[ $# -gt 0 ] || set -- "$DIST"

for f in "$@"; do
	# Re-signing a signed file is refused by osslsigncode; strip first so the
	# script is idempotent and a rebuild-sign-rebuild-sign cycle just works.
	osslsigncode remove-signature -in "$f" -out "$f.stripped" >/dev/null 2>&1 \
		&& mv "$f.stripped" "$f" || rm -f "$f.stripped"

	osslsigncode sign -certs "$CERT" -key "$KEY" -h sha1 \
		-n "MoCoLinux driver" -in "$f" -out "$f.signed" >/dev/null
	mv "$f.signed" "$f"
	osslsigncode verify -CAfile "$CERT" -in "$f" 2>/dev/null | grep -q "Signature verification: ok" \
		|| { echo "$f: signed but verification failed" >&2; exit 1; }
	printf '%-50s signed (SHA-1, test cert)\n' "$f"
done
