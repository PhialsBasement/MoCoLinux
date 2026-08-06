#!/bin/sh
# Build a bootable Manjaro root filesystem for a cooperative guest.
#
# This is the script that produces the shipped root-manjaro.img. It is not
# something a user runs: the whole point is that the image is premade, so that
# somebody on Windows XP x64 downloads a release and runs one executable,
# without a cross toolchain, without pacman, and without a Linux machine
# anywhere in their path. Everything here happens once, on a build host.
#
# It replaces a pile of hand typing. root-arch.img -- the image every
# demonstration in this project has used so far -- was a bootstrap tarball plus
# an evening of commands entered through the guest's own terminal, and none of
# it survives losing the file. That is recorded in TODO as a defect, and this
# exists so the same thing is not true twice.
#
#   mkmanjarorootfs.sh /path/to/root-manjaro.img [size]   build into a new image
#   mkmanjarorootfs.sh /dev/cobd1                         build into a block device
#
# There was a --seed mode here that built a smaller system whose only job was to
# build this one. It is gone: the shipped image carries this script at
# /usr/local/bin/mkmanjarorootfs.sh and a first-boot unit that runs it against
# /dev/cobd1, so the machine being installed onto builds its own root filesystem
# with the same code, from the image that already boots. A second package set
# meant a second thing to keep working, and the path that has actually run end
# to end on this hardware is the one with the full image on cobd0. See
# doc/installer.
#
# The second form is how it runs on the test box. A 6 GB image cannot be pushed
# there -- the transfer agent buffers it and the deploy verifies by reading it
# back, which is 12 GB of churn on a 4 GB machine and has already taken the box
# down -- so the guest builds the next guest's disk in place, on a second cobd
# unit. Anywhere else, build the image and copy it like a normal file.
#
# Requirements on the build host: pacman, and e2fsprogs. Any Arch-derived Linux
# has both, and so does the cooperative guest itself, which is what makes one
# script serve both cases.

set -e

SELF=$(readlink -f "$0")

# --console selects the smaller of two package sets. Not a different code path:
# the same script builds the same filesystem the same way, and only the list of
# packages differs, so there is one definition of what a MoCoLinux root
# filesystem is and one thing to keep working.
#
# It matters because it is the difference between a two-minute install and a
# forty-minute one, for somebody who came here for a Linux shell inside XP
# rather than for KDE. See doc/installer.
CONSOLE=0
if [ "$1" = "--console" ]; then
	CONSOLE=1
	shift
fi

TARGET=${1:?usage: mkmanjarorootfs.sh [--console] <image-or-device> [size]}

# Sized for a desktop plus room to install things afterwards.
SIZE=${2:-6G}

# Manjaro's stable branch, which is the point of choosing Manjaro over Arch for
# a machine somebody else has to live with: package sets that were held back and
# tested together, rather than whatever landed upstream this morning.
#
# Written as explicit per-repo URLs rather than $repo/$arch because this
# configuration is generated through several layers of shell quoting when the
# script runs inside the guest over a serial console, and a literal dollar sign
# is the first thing to be eaten. pacman-mirrors replaces all of this with a
# real ranked mirrorlist once the keyring exists.
#
# Pick a mirror near the machine that is building, and measure rather than
# assume. These two were European because they were the first that answered
# when the script was written, and from Australia that cost a factor of
# twenty-five:
#
#     mirror.aarnet.edu.au        42.6 MB/s
#     ftp.halifax.rwth-aachen.de   1.7 MB/s
#     mirror.datacenter.by         0.8 MB/s
#
# It also produced a wall of TLS handshake failures and curl "transfer too
# slow" aborts on the first large install, which is what a saturated
# trans-continental link does to several concurrent streams -- and which reads
# as a broken NAT rather than a badly chosen server.
#
# pacman-mirrors replaces all of this with a ranked list once the keyring
# exists; these two only have to get that far.
MIRROR1=https://mirror.aarnet.edu.au/pub/manjaro/stable
MIRROR2=https://ftp.halifax.rwth-aachen.de/manjaro/stable

# base pulls systemd, both keyrings, pacman, iproute2 and iputils. It does not
# pull a kernel -- `linux` is an optdepend of base, not a depend -- which is
# exactly right here, because the guest kernel is the vmlinux the host loads and
# a distribution kernel in the image would be dead weight that never boots.
#
# gnupg is explicit: pacman links gpgme, but `pacman-key` is a shell script that
# drives the gpg binary, and without it the keyring cannot be populated at all.
# xcb-util-cursor is not optional and is not pulled in for you.
#
# Since Qt 6.5 the xcb platform plugin refuses to load without it, and nothing
# depends on it, so a package set that installs all of Plasma still misses it.
# What that looks like is not a missing-library message from the loader: the
# plugin is found, fails to initialise, and Qt aborts --
#
#   qt.qpa.plugin: From 6.5.0, xcb-cursor0 or libxcb-cursor0 is needed
#   qt.qpa.plugin: Could not load the Qt platform plugin "xcb"
#
# -- so every Qt application in the image dies on startup, and the abort is what
# then triggers the coredump handling disabled below.
#
# The X clients are the point of this image, not an X server -- there is none in
# the guest and none is wanted. Applications appear as native Windows windows
# through the server on the host, which is also the window manager, so a desktop
# shell would only fight it: no xfwm4, no compositor, no session manager.
#
# ttf-liberation is not optional decoration. Anything Chromium-based, and most
# GTK, exits or renders empty boxes when fontconfig finds no font at all, and a
# package set this small ships none.
#
# lib32 is here because a 32-bit userland is the one thing a stock install of
# this size cannot add later without the multilib repository, and the first
# thing anybody tries is a 32-bit program.
PACKAGES="base manjaro-release manjaro-system pacman-mirrors \
	  gnupg sudo nano inetutils curl networkmanager \
	  zsh zsh-completions \
	  manjaro-kde-settings plasma-meta \
	  konsole kate dolphin firefox ark okular gwenview kcalc \
	  ttf-liberation ttf-dejavu \
	  xorg-xauth xorg-xhost xorg-xrandr xterm \
	  xcb-util-cursor \
	  virtualgl mesa-utils openssh \
	  lib32-glibc lib32-gcc-libs \
	  lib32-virtualgl lib32-libjpeg-turbo lib32-mesa"

# virtualgl is what makes the GPU reachable, and it is the whole reason this
# release exists. The guest has no display of its own: applications draw 2D
# over the X protocol to the X server on Windows, which has no acceleration,
# and their GL used to fall back to llvmpipe -- or fail outright, since
# software GLX against that server dies with GLXBadDrawable. VirtualGL
# redirects the GL onto /dev/dri/renderD128, which is virgl, which is the
# host's real card; the finished frames are pushed into the same X windows. So
# windows stay native and rootless while the drawing happens on the GPU.
#
# lib32-virtualgl and its 32-bit libjpeg-turbo/mesa are not optional the moment
# a 32-bit GL program is run -- Steam's client is the one everybody reaches for.
# vglrun preloads a faker matching the target's word size, so a 32-bit process
# needs the 32-bit libvglfaker/libdlfaker (lib32-virtualgl) and the 32-bit
# libturbojpeg its proxy transport uses (lib32-libjpeg-turbo). Without them the
# preload silently fails, the program falls through to indirect GLX against the
# Windows X server, and dies with the same GLXBadDrawable VirtualGL exists to
# avoid -- which is exactly how it presented: "libturbojpeg.so.0 cannot open",
# "libvglfaker from LD_PRELOAD cannot be preloaded", then GLXBadDrawable. The
# 64-bit set alone shipped in every release before this one.
#
# mesa-utils is glxinfo and glxgears: how anyone checks whether that actually
# happened, rather than trusting it. openssh because a serial console is the
# wrong instrument for diagnosing a graphics stack, and slirp can redirect a
# port to it (see -r on the network daemon).

# The console set. base brings systemd, pacman, both keyrings, iproute2 and
# iputils. e2fsprogs is named rather than relied upon, because building a
# filesystem is a thing this system may be asked to do and inheriting it as
# somebody else's dependency is how that breaks quietly. gnupg because
# pacman-key is a shell script driving the gpg binary, so without it the keyring
# cannot be populated and nothing can be installed afterwards. zsh because the
# console autologs in as a user whose shell this script sets to zsh, and a login
# shell that is not installed is a console that does not work.
CONSOLE_PACKAGES="base manjaro-release manjaro-system pacman-mirrors \
	  gnupg sudo nano inetutils curl e2fsprogs networkmanager \
	  zsh zsh-completions"

if [ "$CONSOLE" = 1 ]; then
	PACKAGES="$CONSOLE_PACKAGES"
fi

MNT=$(mktemp -d)
CONF=$(mktemp)

# Unmounting is not a formality here, it is the whole result.
#
# The first run of this script finished with "installed 179 packages, 885 MB"
# and then failed to unmount, because pacman-key leaves gpg-agent, dirmngr and
# keyboxd running inside the chroot holding sockets. The script exited 0 and the
# image was never flushed; ext4 in data=ordered journals the metadata and not
# the contents, so what survives is a tree of files with the right names and no
# data. A build script that reports success on an unflushed filesystem is worse
# than one that fails.
cleanup() {
	set +e

	# The gpg daemons first, by asking. They are started on demand by
	# pacman-key and nothing else stops them.
	chroot "$MNT" gpgconf --homedir /etc/pacman.d/gnupg --kill all >/dev/null 2>&1

	for d in dev/pts dev sys proc; do
		mountpoint -q "$MNT/$d" && umount "$MNT/$d"
	done

	# Recursive, then lazy, then say so. A lazy unmount still detaches the
	# tree but defers the release, so it is followed by a sync and an
	# explicit warning rather than being treated as success.
	if mountpoint -q "$MNT"; then
		umount -R "$MNT" 2>/dev/null || {
			sync
			umount -l "$MNT" 2>/dev/null
			say "WARNING: $TARGET had to be lazily unmounted -- fsck it"
			UNCLEAN=1
		}
	fi

	sync
	rmdir "$MNT" 2>/dev/null
	rm -f "$CONF"

	# Success is reported here and nowhere else, because until this point
	# the filesystem is not on the disk.
	if [ "${DONE:-0}" = 1 ] && [ "${UNCLEAN:-0}" = 0 ]; then
		say "done, unmounted clean: $TARGET"
	elif [ "${DONE:-0}" = 1 ]; then
		say "built but NOT cleanly unmounted: $TARGET"
	else
		say "did not finish: $TARGET is incomplete"
	fi
}
trap cleanup EXIT

say() { echo "==> $*"; }

# ---------------------------------------------------------------- the filesystem

if [ -b "$TARGET" ]; then
	say "making a filesystem on $TARGET"
else
	say "creating $TARGET ($SIZE) and making a filesystem on it"
	rm -f "$TARGET"
	truncate -s "$SIZE" "$TARGET"
fi

mke2fs -q -F -t ext4 -L manjaro "$TARGET"
mount "$TARGET" "$MNT"

# alpm will not even initialise without these. Its first act is to open its
# database directory, and "could not find or read directory" is what it says
# when the tree is empty -- which reads like a corrupt download rather than a
# missing mkdir. pacstrap creates this skeleton for the same reason.
mkdir -p "$MNT/var/lib/pacman" "$MNT/var/cache/pacman/pkg" "$MNT/var/log" \
	 "$MNT/dev" "$MNT/run" "$MNT/etc/pacman.d/gnupg" "$MNT/tmp"
chmod 1777 "$MNT/tmp"

# ---------------------------------------------------------------- the packages

# Trust on first use, and it should be called that rather than dressed up.
#
# There is no way to verify Manjaro's packages before Manjaro's keyring is
# installed, and the keyring arrives as a package. Arch's own bootstrap tarball
# has the same shape. What closes it is the step after: once the keys are
# populated, signature checking goes to the distribution default and a full
# -Syu re-verifies everything that was laid down here.
say "installing $(echo $PACKAGES | wc -w) package groups (trust-on-first-use)"
#
# ParallelDownloads = 1, and this is not a preference.
#
# Manjaro's own pacman.conf ships 5, and five concurrent TLS streams through
# slirp is what produced a wall of handshake failures and "transfer too slow"
# aborts on the first large install. slirp is a single-threaded 2004 NAT with a
# fixed socket table; curl gives up when a stream drops below its low-speed
# floor, and with five of them competing for one ring pair several always do.
# One at a time is slower in theory and finishes in practice.
#
# multilib is here because lib32 packages live in it, and the bootstrap config
# is the only pacman.conf that exists until manjaro-release installs a real one.
# Without it a 32-bit userland cannot be laid down at all, and pacman aborts the
# whole transaction on the first lib32 target it cannot find.
cat > "$CONF" <<EOF
[options]
HoldPkg = pacman glibc
Architecture = x86_64
ParallelDownloads = 1
SigLevel = Never
LocalFileSigLevel = Never
[core]
Server = $MIRROR1/core/x86_64
Server = $MIRROR2/core/x86_64
[extra]
Server = $MIRROR1/extra/x86_64
Server = $MIRROR2/extra/x86_64
[multilib]
Server = $MIRROR1/multilib/x86_64
Server = $MIRROR2/multilib/x86_64
EOF

pacman --root "$MNT" --config "$CONF" \
       --cachedir "$MNT/var/cache/pacman/pkg" \
       --noconfirm -Sy $PACKAGES

# ---------------------------------------------------------------- the chroot

# Bind mounts before anything runs inside, because post-install scriptlets need
# them and fail without them. A plain `pacman --root` install ends with
# "error: command failed to execute correctly" twice and no indication of which
# scriptlets those were -- sysusers, tmpfiles and locale generation among them,
# so the tree looks complete and is missing its users and its locales.
#
# /run is deliberately NOT bound. Binding the build host's /run into the chroot
# puts the gpg agents' sockets in the host's runtime directory and leaves them
# there, running, holding the mount busy afterwards -- which is how the first
# run of this script ended with an image it could not unmount.
mount --bind /proc "$MNT/proc"
mount --bind /sys  "$MNT/sys"
mount --bind /dev  "$MNT/dev"
mkdir -p "$MNT/dev/pts" && mount --bind /dev/pts "$MNT/dev/pts" 2>/dev/null || true

inside() { chroot "$MNT" /bin/bash -c "$1"; }

# A resolver for the chroot, and this one was silently missing for the life of
# this script.
#
# Nothing installs /etc/resolv.conf -- the booted guest does not need one,
# because Manjaro's nsswitch.conf lists `resolve` and systemd-resolved answers
# through NSS -- but resolved is not running inside a chroot, so every name
# lookup in here fails. What that looks like from pacman is:
#
#   error: failed retrieving file 'extra.db' from <every mirror in the list>
#          : Could not resolve host: <every mirror in the list>
#
# which reads as a bad mirrorlist, and was read as one: the comment above the
# mirror prepend blames servers that "do not resolve at all". They resolve
# fine. Nothing in the chroot could resolve anything.
#
# The cost was the half of trust-on-first-use that closes it. Packages go down
# with SigLevel = Never because the keyring arrives as one of them, and the
# pacman -Syu below is what re-checks every one of them against the real
# keyring afterwards. That step has never once run.
#
# Removed again at the end, so the build host's DNS does not ship inside the
# image.
cp /etc/resolv.conf "$MNT/etc/resolv.conf" 2>/dev/null || \
	printf 'nameserver 1.1.1.1\nnameserver 8.8.8.8\n' > "$MNT/etc/resolv.conf"

say "populating the pacman keyring"
# Not optional, and the failure mode is why it is called out here: without this,
# every package fails verification as TRUST_UNDEFINED *while its signature is
# perfectly good*, so the error points at the download and the download is fine.
# That cost an evening once already.
inside "pacman-key --init"
inside "pacman-key --populate archlinux manjaro"

say "writing a real mirrorlist"
# The bootstrap mirrors above were two hardcoded guesses. pacman-mirrors ranks
# the actual mirror list for the stable branch; if it cannot reach the network
# it leaves what is already there, which still works.
inside "pacman-mirrors --api --set-branch stable" >/dev/null 2>&1 || true

# The mirrorlist, chosen without contacting a single mirror.
#
# pacman-mirrors is not used to pick them. Its default connects to every server
# in the pool and times it; --geoip only changes which pool gets probed, and
# --method random stops the probing at the cost of picking blind -- which is
# how a list ends up headed by whatever the shuffle put first.
#
# Neither is necessary, because Manjaro already publishes what a probe would
# measure. status.json carries each mirror's country, protocols, branch state
# and how long ago it last synced, measured continuously on their side. One
# request to that file yields a ranking that is better than anything a probe
# from behind slirp could produce, and costs exactly one connection -- to the
# API, not to a mirror.
#
# So: take the mirrors that are current on stable, sort them by how recently
# they synced, keep the first ten. Nothing else is ever contacted.
cat > "$MNT/tmp/pick-mirrors.py" <<'PICK'
import json, sys, urllib.request

want = int(sys.argv[1]) if len(sys.argv) > 1 else 10

with urllib.request.urlopen("https://repo.manjaro.org/status.json", timeout=30) as r:
    mirrors = json.load(r)


def staleness(m):
    """Minutes since this mirror last synced; unknown sorts last.

    last_sync is "HH:MM" as published. A mirror that has never synced, or
    whose field is missing or malformed, is not excluded -- it is simply put
    behind every mirror whose freshness is known.
    """
    v = m.get("last_sync")
    try:
        h, mn = str(v).split(":")
        return int(h) * 60 + int(mn)
    except Exception:
        return 1 << 30


# branches is [stable, testing, unstable]; 1 means this mirror carries that
# branch and is current. Anything else is not a stable mirror today.
ok = [m for m in mirrors
      if (m.get("branches") or [0])[0] == 1
      and "https" in (m.get("protocols") or [])
      and m.get("url")]

ok.sort(key=staleness)

if not ok:
    sys.exit("no usable mirrors in status.json")

with open("/etc/pacman.d/mirrorlist", "w") as f:
    f.write("# Generated by mkmanjarorootfs.sh from repo.manjaro.org/status.json.\n")
    f.write("# Ranked by the sync age Manjaro publishes; no mirror was contacted.\n\n")
    for m in ok[:want]:
        f.write("## %s, synced %s ago\n" % (m.get("country", "?"),
                                            m.get("last_sync", "?")))
        f.write("Server = %s$repo/$arch\n" % m["url"])

print(len(ok[:want]))
PICK

if ! inside "python3 /tmp/pick-mirrors.py ${MOCO_MIRRORS:-10}" >/dev/null 2>&1; then
	say "  could not read the mirror API; keeping the bootstrap servers"
fi
rm -f "$MNT/tmp/pick-mirrors.py"

# Put the mirror we measured back at the top of whatever pacman-mirrors chose.
#
# Ranking produced 125 servers, several of which do not resolve at all --
# mirror.bouwhuis.network and mirror.23m.com among them -- and pacman tried
# enough dead ones in a row to abandon the sync entirely, so the final
# re-verification never ran. pacman skips a failed mirror and moves on, but only
# if it reaches a working one before it gives up.
#
# The ranked list is still worth having: it is closer to right for whoever ends
# up running the image than one hardcoded server. This just guarantees the first
# entry is one that answered here.
if [ -f "$MNT/etc/pacman.d/mirrorlist" ]; then
	sed -i "1i Server = $MIRROR1/\$repo/\$arch" \
		"$MNT/etc/pacman.d/mirrorlist"

	#
	# And then keep only the first few.
	#
	# Ranking leaves around 125 Server lines, and the length of that list is
	# not free: pacman walks it on every failure, spending a connection
	# timeout on each dead entry, and it gives up on the sync entirely after
	# enough consecutive failures -- which is the documented reason the
	# re-verification step above once never ran at all. A shorter list is
	# strictly better here: the first entry is a mirror that has actually
	# answered on this machine, the next few are the ranked survivors, and
	# nothing after that was ever going to be reached before pacman quit.
	#
	# Comment lines are kept: the file explains itself to whoever opens it
	# later, and pacman ignores them.
	#
	# MOCO_MIRRORS overrides the count for anyone who wants the whole list.
	awk -v keep="${MOCO_MIRRORS:-10}" '
		/^[[:space:]]*Server[[:space:]]*=/ { if (++n > keep) next }
		{ print }
	' "$MNT/etc/pacman.d/mirrorlist" > "$MNT/etc/pacman.d/mirrorlist.trimmed" &&
		mv "$MNT/etc/pacman.d/mirrorlist.trimmed" \
		   "$MNT/etc/pacman.d/mirrorlist"

	say "  mirrorlist: $(grep -c '^[[:space:]]*Server[[:space:]]*=' \
		"$MNT/etc/pacman.d/mirrorlist") servers"
fi

say "re-verifying against the populated keyring"
# The other half of the trust-on-first-use bargain, and worth stating exactly
# rather than generously -- the comment here used to say a full -Syu
# "re-verifies everything that was laid down here", and it does not.
#
# What it does check: the repository databases, which are signed, and that
# every installed name and version matches what those signed databases say is
# current. A mirror serving tampered or stale content is caught by that.
#
# What it does not check: the signature of each package file already on the
# disk. Those went down under SigLevel = Never and, with nothing left to
# upgrade, pacman has no reason to fetch or verify them again. Closing that
# properly means either installing the keyring first and the rest with
# signatures enforced, or fetching the .sig beside each cached package and
# verifying it by hand. Recorded in TODO rather than half-done here.
#
# Fatal, and it did not used to be. It degraded to a warning, which is the
# wrong shape for this particular step: everything above it was installed with
# SigLevel = Never, so if this does not run then nothing in the image has had
# its signature checked and the script says "done, unmounted clean" anyway.
# That is precisely what happened on every build until the resolver above was
# fixed, and a warning in the middle of a thousand lines of pacman output is
# not how a security property should be reported.
#
# MOCO_ALLOW_UNVERIFIED=1 is for somebody who knows what they are giving up.
if ! inside "pacman -Syu --noconfirm"; then
	if [ "${MOCO_ALLOW_UNVERIFIED:-0}" = 1 ]; then
		say "  WARNING: re-verification failed and was overridden --"
		say "  nothing in this image has had its signature checked"
	else
		say "  re-verification failed: nothing here has been signature-checked."
		say "  Fix the network and run again, or set MOCO_ALLOW_UNVERIFIED=1."
		exit 1
	fi
fi

# ---------------------------------------------------------------- the system

say "configuring the system"

# The installed system inherits the same download limit, for the same reason.
#
# manjaro-release drops in a real pacman.conf with ParallelDownloads = 5, so
# every pacman run inside the booted guest goes back to five concurrent TLS
# streams through slirp -- which is exactly what filled the screen with
# handshake errors and "transfer too slow" aborts the first time somebody
# installed a package by hand. Fixing it only in the bootstrap config would
# leave the trap set for the user rather than the builder.
if grep -q "^ParallelDownloads" "$MNT/etc/pacman.conf" 2>/dev/null; then
	sed -i 's/^ParallelDownloads.*/ParallelDownloads = 1/' "$MNT/etc/pacman.conf"
else
	sed -i 's/^\[options\]/[options]\nParallelDownloads = 1/' "$MNT/etc/pacman.conf"
fi

# No core dumps, and this is not tidiness.
#
# A crash here does not cost one process, it costs the machine. systemd-coredump
# reads the whole address space of the dying program and writes a compressed
# core -- for a Qt application that is a couple of hundred megabytes in and out,
# through cobd, on a guest that has exactly one CPU because cooperative
# virtualisation gives it one. Nothing else runs while that happens. The first
# time it did, the guest looked frozen for minutes and every diagnosis pointed
# somewhere else: the X server, the network, the block layer, the timers.
#
# Storage=none stops the writing, ProcessSizeMax=0 stops the reading, and the
# sysctl covers the window before systemd has claimed the pattern. A guest that
# crashes should lose the program and keep the session.
mkdir -p "$MNT/etc/systemd/coredump.conf.d"
cat > "$MNT/etc/systemd/coredump.conf.d/10-no-coredump.conf" <<'EOF'
[Coredump]
Storage=none
ProcessSizeMax=0
EOF

cat > "$MNT/etc/sysctl.d/50-no-coredump.conf" <<'EOF'
kernel.core_pattern = |/bin/false
EOF

mkdir -p "$MNT/etc/security/limits.d"
cat > "$MNT/etc/security/limits.d/10-no-core.conf" <<'EOF'
*	hard	core	0
EOF

# The dynamic linker cache, which a --root install never builds.
#
# Packages drop fragments into /etc/ld.so.conf.d and rely on their own scriptlet
# running ldconfig; in a chroot that is assembled rather than booted, the last
# word never happens. Nothing complains until something needs a library outside
# the default path -- most visibly /usr/lib32, where a 32-bit program reports
# "you are missing libc.so.6" while lib32-glibc is plainly installed, which
# sends you looking for a missing package that is right there.
inside "ldconfig"

echo "mocolinux" > "$MNT/etc/hostname"
echo "en_US.UTF-8 UTF-8" >> "$MNT/etc/locale.gen"
echo "LANG=en_US.UTF-8" > "$MNT/etc/locale.conf"
inside "locale-gen" >/dev/null

# The root device is whichever cobd unit the host attaches as unit 0, and the
# kernel command line already names it, so fstab only has to not contradict it.
cat > "$MNT/etc/fstab" <<'EOF'
# <file system>	<dir>	<type>	<options>		<dump>	<pass>
/dev/cobd0	/	ext4	rw,relatime		0	1
EOF

# hvc0, not a tty. The guest's console is a hypervisor byte stream -- there is
# no UART and no framebuffer -- so this is the only place a login can appear.
mkdir -p "$MNT/etc/systemd/system/serial-getty@hvc0.service.d"
cat > "$MNT/etc/systemd/system/serial-getty@hvc0.service.d/autologin.conf" <<'EOF'
[Service]
ExecStart=
ExecStart=-/sbin/agetty --autologin mocolinux --noclear %I 115200 linux
EOF
ln -sf /usr/lib/systemd/system/serial-getty@.service \
       "$MNT/etc/systemd/system/getty.target.wants/serial-getty@hvc0.service" \
	2>/dev/null || true

# slirp's layout is fixed and nothing in the guest discovers it, so the address
# is static and lives here rather than in four commands somebody has to remember
# after every boot. Getting this wrong presents as "cannot resolve host", which
# reads as a DNS problem rather than as a missing step.
mkdir -p "$MNT/etc/systemd/network"
cat > "$MNT/etc/systemd/network/10-eth0.network" <<'EOF'
[Match]
Name=eth0

[Network]
Address=10.0.2.15/24
Gateway=10.0.2.2
DNS=10.0.2.3
IPv6AcceptRA=no
LinkLocalAddressing=no
EOF
inside "systemctl enable systemd-networkd" >/dev/null 2>&1 || true
inside "systemctl enable systemd-resolved" >/dev/null 2>&1 || true

# slirp has no IPv6 at all. Left on, the resolver hands back AAAA records first
# and every connection burns a happy-eyeballs timeout before falling back, which
# looks like a slow network rather than a missing protocol.
cat > "$MNT/etc/sysctl.d/40-no-ipv6.conf" <<'EOF'
net.ipv6.conf.all.disable_ipv6 = 1
net.ipv6.conf.default.disable_ipv6 = 1
EOF

# Rootless X, the way the i386 port did it: an X server on the Windows side in
# multiwindow mode, with guest applications appearing as ordinary Windows
# windows. 10.0.2.2 is slirp's gateway alias, and tcp_fconnect() rewrites a
# connection to it into a connection to the host's own loopback -- so this
# reaches an X server listening on 127.0.0.1:6000 on the Windows machine, with
# no port redirection and nothing new in the driver.
#
# OpenGL, and the one wrapper that matters.
#
# The guest has a GPU: /dev/dri/renderD128 is virtio-gpu talking to
# virglrenderer on the Windows side, which executes the GL on the host's real
# card. What it does not have is a display -- applications draw 2D over the X
# protocol to the X server on Windows, and that server has no acceleration.
#
# So the two halves are separated. VirtualGL redirects a program's GL onto the
# render node and pushes the finished frames into the same X windows, which
# leaves the windows native and rootless while the drawing happens on the GT
# 730. Measured on this hardware: glxgears at 132 fps against llvmpipe, and
# Firefox rendering rather than falling back.
#
# The old advice was `glhw`, which set LIBGL_ALWAYS_INDIRECT=1 to send GLX
# protocol to the X server. That path is worse than it looked: capped at GL 1.4
# by the wire protocol, and on this server it does not work at all -- software
# GLX fails with GLXBadDrawable before a frame is drawn. glhw is kept only as a
# shim onto the working path, so anything that already invokes it keeps running.
mkdir -p "$MNT/usr/local/bin"
cat > "$MNT/usr/local/bin/glhw" <<'EOF'
#!/bin/sh
# Superseded by moco-gl, which this now calls.
#
# glhw used to set LIBGL_ALWAYS_INDIRECT=1, sending GLX protocol to the X
# server on Windows. That is limited to GL 1.4 by the wire protocol and on this
# server fails outright with GLXBadDrawable. moco-gl renders on the host's card
# through virgl instead, at full GL 4.2.
echo "glhw: superseded by moco-gl; running that instead" >&2
exec moco-gl "$@"
EOF
chmod 0755 "$MNT/usr/local/bin/glhw"

#
# :0, not 10.0.2.2:0.
#
# :0 is the coxwire shim's unix socket, which carries the X connection through
# guest RAM to the same VcXsrv on the Windows side. 10.0.2.2:0 is the same X
# server reached over slirp instead, and it is eight times slower at a real
# window size -- glxgears 1024x768 measured 5.08 fps that way against 44 over
# the wire, because a 3 MB frame does not fit under slirp's single-stream
# ceiling.
#
# If the shim is ever not running, :0 fails outright rather than falling back.
# That is deliberate: coxwire.service has Restart=always, and a hard failure
# is diagnosable in a way that "the desktop is mysteriously slow" is not.
# Reach the X server directly with DISPLAY=10.0.2.2:0 to prove the difference
# or to work while the shim is down.
#
cat > "$MNT/etc/environment" <<'EOF'
DISPLAY=:0
# OpenGL: run a program through `moco-gl` to render on the host's graphics
# card (virgl on the real GPU, full GL 4.2). Without it a program gets
# llvmpipe in this guest's CPU, or no GL at all -- software GLX against the
# X server on Windows fails with GLXBadDrawable.
#
#     moco-gl firefox
#
# The desktop shortcuts already do this.
EOF

# zsh, and the four files it reads.
#
# Into /etc/skel before the user exists, so useradd -m copies them in with the
# right ownership; root gets its own copy further down.
#
# The first reason is defensive. A zsh that finds no .zshrc runs
# zsh-newuser-install, which asks questions. On a console that autologs in over
# a serial line with nobody watching, that is not a prompt, it is a hang -- and
# it would hang the one way into this guest.
#
# The second is that the four are not interchangeable, and which file a thing
# belongs in is the part people get wrong:
#
#   .zshenv    every zsh, always: interactive or not, login or not. The only one
#              a non-interactive `zsh -c` reads, so anything a script needs goes
#              here -- and nothing slow does, because it runs every time.
#   .zprofile  login shells, after .zshenv.
#   .zshrc     interactive shells. Prompt, history, completion, keys.
#   .zlogin    login shells, after .zshrc, so it runs last of the four.
mkdir -p "$MNT/etc/skel"

cat > "$MNT/etc/skel/.zshenv" <<'SKEL'
# Read by every zsh, including non-interactive ones. Environment only.

# The X server is on the Windows side, reached through the coxwire shim's
# socket rather than over slirp -- see the note on /etc/environment above for
# what that is worth. /etc/environment carries it too, but only reaches
# sessions that go through PAM, and `su -` here does not.
export DISPLAY=:0

export EDITOR=nano
export PATH="$HOME/.local/bin:$PATH"

# OpenGL renders in this guest's CPU by default -- complete, and slow, because
# there is no GPU here. Run a single program through `glhw` to use the host's
# graphics card instead; it is capped at GL 1.4 by the GLX protocol.
SKEL

cat > "$MNT/etc/skel/.zprofile" <<'SKEL'
# Login shells, between .zshenv and .zshrc. Deliberately near-empty: environment
# belongs in .zshenv, where non-interactive shells see it too.
SKEL

cat > "$MNT/etc/skel/.zshrc" <<'SKEL'
# Interactive shells only.

HISTFILE=~/.zsh_history
HISTSIZE=10000
SAVEHIST=10000
setopt HIST_IGNORE_DUPS SHARE_HISTORY EXTENDED_HISTORY
setopt AUTO_CD INTERACTIVE_COMMENTS

autoload -Uz compinit && compinit -d ~/.cache/zcompdump
zstyle ':completion:*' menu select
zstyle ':completion:*' matcher-list 'm:{a-z}={A-Za-z}'
autoload -Uz colors && colors

# The machine is named in the prompt deliberately. This shell is reached over a
# console from another computer, usually sitting next to a shell on that
# computer, and two identical prompts is how you run the wrong command.
PROMPT='%F{cyan}%n@mocolinux%f %F{yellow}%~%f %# '

alias ls='ls --color=auto'
alias grep='grep --color=auto'
alias ll='ls -lh'
alias la='ls -lha'

# Emacs bindings and the keys a serial console does not bind for itself.
bindkey -e
bindkey '^[[A' up-line-or-search
bindkey '^[[B' down-line-or-search
bindkey '^[[H' beginning-of-line
bindkey '^[[F' end-of-line
bindkey '^[[3~' delete-char
SKEL

cat > "$MNT/etc/skel/.zlogin" <<'SKEL'
# Login shells, after .zshrc -- the last of the four, so the shell is fully set
# up by the time anything here runs.
SKEL

# A user, because running a desktop as root is how people learn not to -- and
# because a lot of software simply refuses. Steam checks `id -u` and exits.
#
# DISPLAY has to be set here as well as in /etc/environment. `su -` on Manjaro
# does not run pam_env, so /etc/environment is never read for that session and
# an X client started after `su - mocolinux` fails with "cannot open display" --
# which reads like the X server is unreachable when the connection is fine and
# the variable is simply absent.
inside "useradd -m -G wheel -s /bin/zsh mocolinux" >/dev/null 2>&1 || true
inside "echo 'mocolinux:mocolinux' | chpasswd" >/dev/null 2>&1 || true
# bash stays installed and still works, so it gets DISPLAY too: `bash -lc`
# should not find a different environment from the login shell.
for rc in "$MNT/home/mocolinux/.bashrc" "$MNT/root/.bashrc"; do
	echo 'export DISPLAY=:0' >> "$rc"
done

# root reads the same zsh files but keeps bash as its login shell. A broken
# interactive config must never be the thing standing between an operator and a
# root prompt.
for f in .zshenv .zprofile .zshrc .zlogin; do
	cp "$MNT/etc/skel/$f" "$MNT/root/$f"
done
#
# No password prompt for wheel.
#
# The console autologs in as this user, so a prompt means every administrative
# command on a single-user development guest stops to ask for a password that is
# set three lines above in this same script. It buys nothing, and it breaks
# unattended use of the serial console.
echo "%wheel ALL=(ALL:ALL) NOPASSWD: ALL" > "$MNT/etc/sudoers.d/10-wheel"
chmod 0440 "$MNT/etc/sudoers.d/10-wheel"

# ---------------------------------------------------------------- the builder

# The image carries the thing that builds the next one.
#
# This used to be gated behind --seed, on the theory that a release ships a
# small system whose only job is to build the real one. That is gone: the
# machine being installed onto boots this image and builds its own root
# filesystem on the second disk, with the same script, and the path with the
# full image on cobd0 is the one that has actually run end to end here.
#
# run-manjaro.bat already keeps the previous root attached as cobd1 for the same
# reason in reverse: if the new system will not boot, the one that built it
# still will.

say "installing the GPU wrapper"

# One command that runs an application on the host's GPU, with the three
# settings that are not optional and not guessable.
#
# VGL_COMPRESS=proxy is the one that matters. With a remote DISPLAY -- which
# this always is, the X server being on Windows -- VirtualGL otherwise selects
# its own transport and tries to connect to a vglclient on the display host.
# Nothing is listening, so it blocks in connect() forever: no error, no
# timeout, no log line, just an application that starts and never draws. An
# evening was lost to that. "proxy" means send the frames down the X
# connection that is already open, which needs no client at all.
#
# VGL_PROBEGLX=0 skips VirtualGL's probe of the 2D server for stereo visuals.
# That probe asks the X server for GLX capabilities, and this one answers in a
# way that hangs the probe -- again silently.
#
# -d egl0 selects the EGL device by index. Naming the DRM node directly
# (-d /dev/dri/renderD128) is refused with "Invalid EGL device": virglrenderer's
# node does not advertise itself through EGL_EXT_device_drm the way VirtualGL
# expects, but it is there and it is first.
install -Dm 0755 /dev/stdin "$MNT/usr/local/bin/moco-gl" <<'MOCOGL'
#!/bin/sh
# Run a program with its OpenGL on the host's GPU.
#
#   moco-gl firefox
#   moco-gl glxgears
#
# Without this wrapper an application gets software rendering at best, and at
# worst no GL at all -- software GLX against the Windows X server fails with
# GLXBadDrawable.
if [ $# -eq 0 ]; then
	echo "usage: moco-gl <program> [args...]" >&2
	exit 2
fi

: "${DISPLAY:=:0}"
export DISPLAY

exec env VGL_PROBEGLX=0 VGL_COMPRESS=proxy vglrun -d egl0 "$@"
MOCOGL

# sshd, because diagnosing this system through a serial console is worse than
# the problems being diagnosed. Reachable once the network daemon is given a
# redirection: colinux-slirp-net-daemon -R -r tcp:2222:22
inside "systemctl enable sshd" >/dev/null 2>&1 || \
	say "  WARNING: could not enable sshd"

# ------------------------------------------------------------------- the wire
#
# CoXWire: the X connection carried through guest RAM instead of slirp. The
# shim owns /tmp/.X11-unix/X0, so an application only has to use DISPLAY=:0 --
# libX11, VirtualGL and the application itself are unchanged. Measured on the
# M92p, glxgears 1024x768 with VGL_SPOIL=0: 5.08 fps over slirp, 44 over this.
#
# The binary is COPIED, never compiled here.
#
# This script's most important caller is the seed image building the machine's
# real root filesystem on first boot, and that guest has no compiler -- so a
# build step would mean every installed system silently ends up without the
# wire. It propagates the same way this script propagates itself: the running
# system's own /usr/local/bin/coxwire goes into the image it is building, so
# each generation carries the next one's copy.
#
# Compiling is the last resort, for a developer seeding the very first image
# on a machine that has the source and a compiler. -march=x86-64 with the
# glibc note stripped, both deliberate: a build host newer than the target (a
# CachyOS box defaults to x86-64-v3) otherwise produces a binary that dies
# with "CPU ISA level is lower than required" before main() on the Ivy Bridge
# this runs on.
COXWIRE_BIN=""
for b in /usr/local/bin/coxwire \
	 "$(dirname "$SELF")/../../dist-x64/coxwire"; do
	[ -x "$b" ] && { COXWIRE_BIN=$b; break; }
done

if [ -z "$COXWIRE_BIN" ]; then
	COXWIRE_SRC="$(dirname "$SELF")/../src/colinux/user/coxwire/coxwire-guest.c"
	if [ -f "$COXWIRE_SRC" ] && command -v cc >/dev/null 2>&1; then
		say "no prebuilt X wire shim; building one"
		if cc -O2 -march=x86-64 -mtune=generic -o "$MNT/usr/local/bin/coxwire" \
		      "$COXWIRE_SRC" -lpthread 2>/dev/null; then
			objcopy --remove-section=.note.gnu.property \
				"$MNT/usr/local/bin/coxwire" 2>/dev/null || true
			COXWIRE_BIN=$MNT/usr/local/bin/coxwire
		fi
	fi
fi

if [ -n "$COXWIRE_BIN" ]; then
	install -Dm 0755 "$COXWIRE_BIN" "$MNT/usr/local/bin/coxwire"
	say "installed the X wire's guest shim"

	cat > "$MNT/etc/systemd/system/coxwire.service" <<'COXWIRE'
[Unit]
Description=CoXWire: the X connection over guest RAM
After=network.target

[Service]
Type=simple
ExecStart=/usr/local/bin/coxwire
Restart=always
RestartSec=2

[Install]
WantedBy=multi-user.target
COXWIRE
	inside "systemctl enable coxwire" >/dev/null 2>&1 || \
		say "  WARNING: could not enable coxwire"
else
	# Loud, because the symptom otherwise appears much later and looks
	# like something else entirely: X clients still work over slirp, so
	# the desktop comes up and is simply eight times slower.
	say "  WARNING: no coxwire binary found -- X will fall back to slirp"
fi

say "installing the builder and its first-boot unit"

# This script, inside the image it just built. The system that gets installed
# on somebody's machine is therefore built by the same code that built the
# seed, from the same file -- not a copy that drifted.
install -Dm 0755 "$SELF" "$MNT/usr/local/bin/mkmanjarorootfs.sh"

# The first-boot unit's wrapper.
#
# It runs the builder against the second disk and translates its output into
# the marker vocabulary doc/installer specifies, so the host side reads a
# stream rather than scraping a shell prompt. The reason that matters is two
# lines up in this very file: the mirror configuration is written as literal
# per-repo URLs because a dollar sign does not survive the layers of quoting
# involved in driving this script through a serial console. Anything that
# needs the host to compose shell has the same problem.
#
# Everything the builder prints goes through untouched except its own "==> "
# stage lines, which become MOCO:STAGE. A reader that does not care about
# markers still sees an ordinary build log.
cat > "$MNT/usr/local/bin/mocolinux-setup" <<'SETUP'
#!/bin/bash
# Build this machine's root filesystem on the second disk, on first boot.
#
# Output goes to hvc0 by name, and not to /dev/console, which is the mistake
# this line replaces. There is no framebuffer and no UART in this guest: the
# only channel out is the hypervisor byte stream that colinux-daemon --console
# serves, and that is hvc0. /dev/console is whatever the kernel was told to
# use with console= on its command line, which is not this -- so every marker
# written there went nowhere at all, and the host had a progress protocol with
# nothing on the other end of it.
#
# A file first, the console second.
#
# The file is what makes this diagnosable. Console output is a stream with no
# history: anything printed before a client attaches, or while one is busy, is
# gone, and a twenty-minute build watched through a terminal that comes and
# goes shows whatever happened to be passing at the time. The log persists, so
# progress can be read at any point with a single `tail`, and a failed build
# can be examined afterwards instead of reconstructed.
# The log is the channel. Not the console.
#
# hvc0 is the only byte stream out of this guest, and it is already carrying a
# login shell. Writing the build to it as well put two writers on one tty, and
# the host read whichever won -- measured, six minutes of build output reached
# the log and none of it reached the console, while the capture filled with the
# shell's prompt redraws and bracketed-paste escapes.
#
# Standing the getty down would fix that collision and create a worse one. The
# host reads progress by running a command in the guest and matching a sentinel
# in the reply, which needs a shell on hvc0; a build writing to the same tty
# corrupts those replies mid-word, which is what "ddmesg" and "print ff" in this
# project's own transcripts are. Either the console carries the build or it
# carries the conversation, and the conversation is what the host needs.
#
# So: a file, and the host tails it. That is also strictly more than a stream can
# offer. A stream has no history, so anything written before the wizard attaches,
# or while it reconnects, is simply gone -- and this build takes long enough that
# an operator will attach late, detach, and come back. The file can be re-read
# from the beginning at any point, which makes progress idempotent to observe and
# a failed build examinable afterwards instead of reconstructed.
LOG=/var/log/mocolinux-setup.log
exec >> "$LOG" 2>&1

TARGET=/dev/cobd1

# Eight, not the seven doc/installer lists: the seven build stages, and then the
# guest checking its own work. That verification is not a formality -- a
# block-layer bug once produced an image whose own package database was binary
# noise -- so it is a stage the user can watch rather than something that happens
# in silence after the progress bar has already filled.
STAGES=8
MIRROR=https://mirror.aarnet.edu.au/pub/manjaro/stable/core/x86_64/core.db

# console  -- the smaller package set, so the built system is a shell and a
#             network and nothing else, ready in a couple of minutes
# desktop  -- the full set, which is the 2.5 GB download
SET=desktop
for word in $(cat /proc/cmdline); do
	case "$word" in
	mocolinux.set=*) SET=${word#mocolinux.set=} ;;
	esac
done

fail() {
	echo "MOCO:FAIL $1 $2"
	exit 1
}

[ -b "$TARGET" ] || fail nodisk "no second disk attached at $TARGET"

echo "MOCO:STAGE 1/$STAGES checking the network"
# Ahead of pacman deliberately. An unreachable network here is one line that
# names the problem; the same failure inside pacman is a wall of TLS and
# "transfer too slow" errors that reads as a broken mirror or a broken NAT.
curl -4 -s -I --max-time 30 -o /dev/null "$MIRROR" \
	|| fail network "cannot reach the Manjaro mirror -- is the network bridge running?"

ARGS=""
[ "$SET" = console ] && ARGS="--console"

# The exit status has to come out of the pipeline, and `set -o pipefail` is
# not in POSIX sh, so the builder records its own.
RC=/run/mocolinux-setup.rc
rm -f "$RC"
{ /usr/local/bin/mkmanjarorootfs.sh $ARGS "$TARGET"; echo $? > "$RC"; } 2>&1 |
while IFS= read -r line; do
	case "$line" in
	"==> "*)
		# A table, not a counter.
		#
		# Counting "==> " lines was the obvious thing and it is wrong: the
		# builder prints one for every step it takes, which is seventeen
		# and not seven, so the first real run reported "MOCO:STAGE 14/7"
		# and then 15, 16 and 17 of 7. A progress bar fed that is worse
		# than no progress bar, because it looks like it is working.
		#
		# So the numbered stages are the ones doc/installer names, matched
		# on the builder's own text. Everything else the builder announces
		# is real progress worth showing, but it belongs on the log line
		# rather than in the stage count -- MOCO:INFO, which the wizard
		# puts in the log pane and not in the stage label.
		#
		# Matched on a distinctive fragment rather than the whole line,
		# because these strings carry counts and device names that change
		# from run to run. If the builder's wording changes, a stage stops
		# being recognised and its text arrives as INFO: the build still
		# works and still reports, it just loses one number. That is the
		# right way for this to fail.
		text=${line#==> }
		case "$text" in
		*"making a filesystem"*|*"creating "*)
			echo "MOCO:STAGE 2/$STAGES making the filesystem" ;;
		*"installing "*"package groups"*)
			echo "MOCO:STAGE 3/$STAGES downloading and installing" ;;
		*"populating"*keyring*|*"pacman-key"*)
			echo "MOCO:STAGE 4/$STAGES populating the keyring" ;;
		*"ranking mirrors"*|*"pacman-mirrors"*)
			echo "MOCO:STAGE 5/$STAGES ranking mirrors" ;;
		*"re-verifying"*)
			echo "MOCO:STAGE 6/$STAGES re-verifying every package" ;;
		*"configuring"*)
			echo "MOCO:STAGE 7/$STAGES configuring" ;;
		*)
			echo "MOCO:INFO $text" ;;
		esac
		;;
	*)
		echo "$line"
		;;
	esac
done

[ "$(cat $RC 2>/dev/null)" = 0 ] || fail build "building the root filesystem failed"

# Verify before saying it worked. A block-layer bug in this port once produced
# an image whose own package database read back as binary noise, so "the
# script exited 0" is not the same as "there is a system on that disk".
echo "MOCO:STAGE $STAGES/$STAGES verifying"
VERIFY=/mnt/new
mkdir -p "$VERIFY"
mount "$TARGET" "$VERIFY" || fail mount "the new filesystem will not mount"

MISSING=$(pacman --root "$VERIFY" -Qk 2>&1 | grep -v "0 missing files" || true)
[ -x "$VERIFY/usr/lib/systemd/systemd" ] || { umount "$VERIFY"; fail noinit "no systemd in the new filesystem"; }
[ -f "$VERIFY/etc/fstab" ] || { umount "$VERIFY"; fail nofstab "no fstab in the new filesystem"; }
umount "$VERIFY"

if [ -n "$MISSING" ]; then
	echo "$MISSING"
	fail damaged "installed packages are missing files -- the image is not sound"
fi

# Only now, and only after the filesystem is unmounted, which is the point at
# which its contents are actually on the disk.
e2fsck -n -f "$TARGET" > /dev/null 2>&1 || fail fsck "the new filesystem does not check clean"

systemctl disable mocolinux-setup.service > /dev/null 2>&1
echo "MOCO:OK"
SETUP
chmod 0755 "$MNT/usr/local/bin/mocolinux-setup"

# Wanted by multi-user.target rather than run from the console, so it starts
# whether or not anybody is attached, and after the network is configured.
#
# It stays enabled if it fails, so the next boot tries again -- which is what
# makes a retry work without the host knowing anything about what went wrong.
# On success it disables itself, and the seed becomes an ordinary system.
cat > "$MNT/usr/lib/systemd/system/mocolinux-setup.service" <<'UNIT'
[Unit]
Description=Build the MoCoLinux root filesystem on the second disk
After=systemd-networkd-wait-online.service network-online.target
Wants=network-online.target
ConditionPathExists=/dev/cobd1

# The getty is deliberately left alone. An earlier version of this unit stopped
# it, to keep the console clear for markers; that was backwards. The build writes
# to a log now, and the host reads that log by running tail through the shell the
# getty provides -- so the getty is the transport, not a competitor for it.

[Service]
Type=oneshot
ExecStart=/usr/local/bin/mocolinux-setup
RemainAfterExit=yes
TimeoutStartSec=infinity
StandardOutput=null
StandardError=null

[Install]
WantedBy=multi-user.target
UNIT
inside "systemctl enable mocolinux-setup.service" > /dev/null 2>&1 || \
	say "  WARNING: could not enable mocolinux-setup.service"

# The package cache stays. It is what makes a retry cheap -- a mirror dropping
# part way through is the likeliest failure anyone will meet, and pacman picking
# up from a warm cache is the difference between a retry and an abandonment.
#
# Shrinking an image for release -- emptying the cache, then zeroing the free
# space so that ext4's leftovers compress instead of shipping as noise -- is a
# separate step against a finished image, not something to do to the working
# copy while building it.

# ---------------------------------------------------------------- done

# The build host's resolver does not ship. The symlink is what a systemd
# system expects to find here, and it costs nothing on a guest that resolves
# through NSS anyway.
rm -f "$MNT/etc/resolv.conf"
ln -sf /run/systemd/resolve/stub-resolv.conf "$MNT/etc/resolv.conf"

sync
# df rather than du, because proc, sys and dev are still bound here: du walks
# into the host's /proc and reports its size along with a page of errors about
# pids that vanished while it was reading them. df asks the filesystem, which
# is the number that was wanted in the first place.
say "installed $(ls "$MNT/var/lib/pacman/local" | wc -l) packages, $(df -m "$MNT" | awk 'NR==2 {print $3}') MB"

# The unmount happens in the EXIT trap, so the success line cannot be printed
# before the data is actually on the disk. It is printed by the trap instead.
DONE=1
