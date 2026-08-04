#!/bin/sh
# Build the guest's ext4 root filesystem image.
#
# The guest cannot format its own disk -- it has no userspace until it has a
# root filesystem, which is the thing being built -- so the image is made here
# and shipped whole. mke2fs -d populates it directly from a directory, which
# means no loop mount and therefore no root privileges.
#
#   mkrootfs.sh [size-mb]        default 512
#
# The result is dist-x64/root.img, deployed like any other artifact and
# attached with `colinux-daemon --cobd0 \DosDevices\F:\xfer\root.img`. Pointing cobd0
# at a raw partition instead is a path change and nothing else; the driver
# opens whatever it is given.

set -e

# Under fakeroot, so the files land owned by root rather than by whoever ran
# this. It costs nothing and it is not cosmetic: /etc/passwd and the busybox
# binary owned by uid 1000 is a filesystem that behaves differently the moment
# anything in the guest drops privileges.
if [ -z "$FAKEROOTKEY" ] && command -v fakeroot >/dev/null 2>&1; then
	exec fakeroot "$0" "$@"
fi

DIST=/mnt/big-bricks/RProject/MoCoLinux/dist-x64
WORK=${TMPDIR:-/tmp}/mocolinux-rootfs.$$
SIZE=${1:-512}
BUSYBOX=${BUSYBOX:-$DIST/busybox}

[ -f "$BUSYBOX" ] || {
	echo "no busybox at $BUSYBOX"
	echo "fetch a static one, e.g. from busybox.net/downloads/binaries"
	exit 1
}

mkdir -p "$WORK/root"
trap 'rm -rf "$WORK"' EXIT

cd "$WORK/root"
mkdir -p bin sbin etc proc sys dev tmp root var/log usr/bin usr/sbin
chmod 1777 tmp

cp "$BUSYBOX" bin/busybox
chmod 755 bin/busybox

# Every applet busybox knows, as a symlink to it. Done from the list the
# binary itself reports rather than a list written here, which would drift.
for applet in $("$BUSYBOX" --list); do
	case "$applet" in
	busybox|init|linuxrc) continue ;;
	esac
	ln -sf /bin/busybox "bin/$applet" 2>/dev/null || true
done

ln -sf /bin/busybox sbin/init
ln -sf /bin/busybox init

# No device nodes in the image, deliberately.
#
# The kernel is built with CONFIG_DEVTMPFS_MOUNT, so it mounts devtmpfs on
# /dev itself inside prepare_namespace -- after the root filesystem and before
# it opens /dev/console for init. Every node the guest needs, including the
# cobd disks, therefore appears at boot with the major the kernel actually
# allocated. Nodes written here would need mknod and so root privileges, and
# cobd's major is assigned dynamically, so any number baked in would be a
# guess that goes stale.

cat > etc/inittab <<'EOF'
::sysinit:/etc/rc
# A shell on the cooperative console. hvc0 is the guest end of the two rings
# the host serves over TCP (colinux-daemon --console PORT), so this is the
# terminal a person actually connects to. askfirst rather than respawn so it
# prints a prompt and waits, which is also how you can tell the input
# direction works before typing anything useful.
hvc0::respawn:/bin/sh
::respawn:/bin/sh
::ctrlaltdel:/sbin/reboot
::shutdown:/bin/umount -a -r
EOF

cat > etc/rc <<'EOF'
#!/bin/sh
mount -t proc  proc  /proc
mount -t sysfs sysfs /sys
echo
echo "=============================================="
echo " MoCoLinux: userspace is running on Windows XP x64"
echo "=============================================="
echo "kernel:  $(uname -a)"
echo "uptime:  $(cat /proc/uptime)"
echo "memory:"
free 2>/dev/null || head -3 /proc/meminfo
echo "root:    $(mount | grep ' / ')"
echo
EOF
chmod 755 etc/rc

cat > etc/fstab <<'EOF'
/dev/cobd0	/	ext4	defaults	0 1
proc		/proc	proc	defaults	0 0
sysfs		/sys	sysfs	defaults	0 0
EOF

echo "mocolinux" > etc/hostname
printf 'root::0:0:root:/root:/bin/sh\n' > etc/passwd
printf 'root:x:0:\n' > etc/group

cd "$WORK"

# 4 KB blocks, no journal checksum seeds or 64bit features the kernel's
# ext4 might refuse: metadata_csum_seed and orphan_file are recent enough
# that pinning the feature set is cheaper than finding out at mount time.
rm -f "$DIST/root.img"
mke2fs -q -t ext4 -L mocoroot -b 4096 \
       -O ^metadata_csum_seed,^orphan_file \
       -d root -F "$DIST/root.img" "${SIZE}m"

echo "dist-x64/root.img  ${SIZE} MB"
ls -la "$DIST/root.img"
