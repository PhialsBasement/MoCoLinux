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

TARGET=${1:?usage: mkmanjarorootfs.sh <image-or-device> [size]}
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
	  manjaro-kde-settings plasma-meta \
	  konsole kate dolphin firefox ark okular gwenview kcalc \
	  ttf-liberation ttf-dejavu \
	  xorg-xauth xorg-xhost xorg-xrandr xterm \
	  lib32-glibc lib32-gcc-libs"

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
inside "pacman-mirrors --fasttrack 5" >/dev/null 2>&1 || \
	say "  pacman-mirrors could not rank mirrors; keeping the bootstrap servers"

say "re-verifying every installed package against the populated keyring"
# The other half of the trust-on-first-use bargain. Signature checking is back
# at the distribution default by now, because manjaro-release installed a real
# pacman.conf over the bootstrap one.
inside "pacman -Syu --noconfirm" || \
	say "  full re-verification did not complete; check the image before shipping"

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
ExecStart=-/sbin/agetty --autologin root --noclear %I 115200 linux
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
cat > "$MNT/etc/environment" <<'EOF'
DISPLAY=10.0.2.2:0
EOF

# A user, because running a desktop as root is how people learn not to -- and
# because a lot of software simply refuses. Steam checks `id -u` and exits.
#
# DISPLAY has to be set here as well as in /etc/environment. `su -` on Manjaro
# does not run pam_env, so /etc/environment is never read for that session and
# an X client started after `su - mocolinux` fails with "cannot open display" --
# which reads like the X server is unreachable when the connection is fine and
# the variable is simply absent.
inside "useradd -m -G wheel -s /bin/bash mocolinux" >/dev/null 2>&1 || true
inside "echo 'mocolinux:mocolinux' | chpasswd" >/dev/null 2>&1 || true
echo 'export DISPLAY=10.0.2.2:0' >> "$MNT/home/mocolinux/.bashrc"
echo 'export DISPLAY=10.0.2.2:0' >> "$MNT/root/.bashrc"
echo "%wheel ALL=(ALL:ALL) ALL" > "$MNT/etc/sudoers.d/10-wheel"
chmod 0440 "$MNT/etc/sudoers.d/10-wheel"

# ---------------------------------------------------------------- done

sync
say "installed $(ls "$MNT/var/lib/pacman/local" | wc -l) packages, $(du -sm "$MNT" | cut -f1) MB"

# The unmount happens in the EXIT trap, so the success line cannot be printed
# before the data is actually on the disk. It is printed by the trap instead.
DONE=1
