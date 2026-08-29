#!/usr/bin/env bash
# Builds a bootable Debian rootfs (Python + numpy + faiss baked in, plus a
# systemd service that runs a workload script on every boot) directly onto
# ROOTFS_IMG, and extracts the kernel/initrd QEMU needs to boot it via
# -kernel/-initrd (see kvm/vms.yaml's append line for the matching kernel
# cmdline). Invoked by `make guest-image` -- see the Makefile for the env
# vars this expects and their defaults.
#
# All writes here go straight to ROOTFS_IMG on the host filesystem via
# debootstrap/chroot -- a completely different path from nbd_server, which
# discards every write a *running* guest makes to this same file (see
# disk/sim.hpp). That's why the guest boots with systemd.volatile=yes: it
# never depends on a write actually landing on disk mid-run.
#
# Must run as root (debootstrap/mount/chroot need it) on a Debian/Ubuntu
# Linux host with debootstrap installed.
set -euo pipefail

ROOTFS_IMG=${ROOTFS_IMG:?ROOTFS_IMG not set}
ROOTFS_SIZE=${ROOTFS_SIZE:-3G}
DISTRO=${DISTRO:-bookworm}
ARCH=${ARCH:-amd64}
MIRROR=${MIRROR:-http://deb.debian.org/debian}
WORKLOAD_SCRIPT=${WORKLOAD_SCRIPT:?WORKLOAD_SCRIPT not set}
KERNEL_OUT=${KERNEL_OUT:?KERNEL_OUT not set}
INITRD_OUT=${INITRD_OUT:?INITRD_OUT not set}

if [[ $EUID -ne 0 ]]; then
    echo "build_image.sh must run as root (debootstrap/mount/chroot need it)" >&2
    exit 1
fi
for tool in debootstrap mkfs.ext4 losetup chroot; do
    command -v "$tool" >/dev/null || { echo "missing required tool: $tool" >&2; exit 1; }
done
[[ -f "$WORKLOAD_SCRIPT" ]] || { echo "workload script not found: $WORKLOAD_SCRIPT" >&2; exit 1; }

MNT=$(mktemp -d)
LOOP=""

cleanup() {
    set +e
    mountpoint -q "$MNT/dev" && umount "$MNT/dev"
    mountpoint -q "$MNT/proc" && umount "$MNT/proc"
    mountpoint -q "$MNT/sys" && umount "$MNT/sys"
    mountpoint -q "$MNT" && umount "$MNT"
    [[ -n "$LOOP" ]] && losetup -d "$LOOP"
    rmdir "$MNT"
}
trap cleanup EXIT

mkdir -p "$(dirname "$ROOTFS_IMG")" "$(dirname "$KERNEL_OUT")" "$(dirname "$INITRD_OUT")"
truncate -s "$ROOTFS_SIZE" "$ROOTFS_IMG"
mkfs.ext4 -F -q "$ROOTFS_IMG"

LOOP=$(losetup -f --show "$ROOTFS_IMG")
mount "$LOOP" "$MNT"

debootstrap --arch="$ARCH" "$DISTRO" "$MNT" "$MIRROR"

cp /etc/resolv.conf "$MNT/etc/resolv.conf"
mount --bind /dev "$MNT/dev"
mount -t proc proc "$MNT/proc"
mount -t sysfs sysfs "$MNT/sys"

chroot "$MNT" env DEBIAN_FRONTEND=noninteractive apt-get update
chroot "$MNT" env DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
    linux-image-"$ARCH" systemd-sysv ca-certificates python3 python3-pip python3-numpy
chroot "$MNT" pip3 install --break-system-packages --no-cache-dir faiss-cpu

mkdir -p "$MNT/opt/workload"
cp "$WORKLOAD_SCRIPT" "$MNT/opt/workload/run.py"

# Powers the guest off once the workload exits (success or failure) so that
# a host-side `kvm/launch.py --wait` can detect completion by watching for
# the QEMU process to exit, rather than polling the serial log's output.
cat > "$MNT/opt/workload/run.sh" <<'EOF'
#!/bin/sh
python3 /opt/workload/run.py
poweroff
EOF
chmod +x "$MNT/opt/workload/run.sh"

cat > "$MNT/etc/systemd/system/workload.service" <<'EOF'
[Unit]
Description=Boot-time workload

[Service]
Type=oneshot
ExecStart=/opt/workload/run.sh
StandardOutput=journal+console
StandardError=journal+console

[Install]
WantedBy=multi-user.target
EOF
chroot "$MNT" systemctl enable workload.service

# Autologin on the serial console so kvm/launch.py's display:none serial log
# (<log_dir>/<name>.serial.log) shows the workload's output with no ssh/network
# needed -- root has no password, so this is only safe because network.type
# is none by default in kvm/vms.yaml.
mkdir -p "$MNT/etc/systemd/system/serial-getty@ttyS0.service.d"
cat > "$MNT/etc/systemd/system/serial-getty@ttyS0.service.d/autologin.conf" <<'EOF'
[Service]
ExecStart=
ExecStart=-/sbin/agetty --autologin root --keep-baud 115200,38400,9600 %I $TERM
EOF
chroot "$MNT" passwd -d root

KVER=$(chroot "$MNT" bash -c "ls /boot | grep '^vmlinuz-' | sort -V | tail -1")
INITRD_NAME=$(chroot "$MNT" bash -c "ls /boot | grep '^initrd.img-' | sort -V | tail -1")
cp "$MNT/boot/$KVER" "$KERNEL_OUT"
cp "$MNT/boot/$INITRD_NAME" "$INITRD_OUT"

echo "guest image built: $ROOTFS_IMG"
echo "kernel:  $KERNEL_OUT"
echo "initrd:  $INITRD_OUT"
echo "workload: $WORKLOAD_SCRIPT -> /opt/workload/run.py (runs once per boot via workload.service)"
