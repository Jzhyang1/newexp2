#!/usr/bin/env bash
# Builds a bootable Debian rootfs (Python + numpy + faiss baked in, plus a
# systemd service that runs a workload script on every boot) directly onto
# ROOTFS_IMG, and extracts the kernel/initrd QEMU needs to boot it via
# -kernel/-initrd (see kvm/vms.yaml's append line for the matching kernel
# cmdline). Invoked by `make guest-image` -- see the Makefile for the env
# vars this expects and their defaults.
#
# If YCSB_ENABLE=1 (the Makefile's default), also downloads YCSB + a JRE and
# pre-loads a SQLite dataset under /opt/workload, all baked into ROOTFS_IMG
# here so kvm/guest/workloads/ycsb_bench.py has a real on-disk dataset to
# run reads against at boot.
#
# If GUPS_ENABLE=1, also writes a real (non-sparse) zero-filled table file
# under /opt/workload here, so kvm/guest/workloads/gups_bench.py -- a
# disk-based analogue of the HPC Challenge "GUPS" RandomAccess benchmark --
# has a real on-disk table to issue random-offset reads against at boot.
# Independent of YCSB_ENABLE; both datasets can be baked into the same
# image at once (WORKLOAD_SCRIPT picks which one actually runs at boot).
#
# All writes here go straight to ROOTFS_IMG on the host filesystem via
# debootstrap/chroot -- a completely different path from nbd_server, which
# discards every write a *running* guest makes to this same file (see
# disk/sim.hpp). That's why the guest boots with systemd.volatile=state
# (root, including /opt, stays read-only from the real disk; only /var is a
# tmpfs overlay) -- NOT systemd.volatile=yes, which would tmpfs everything
# outside /usr and wipe /opt/workload on every boot.
#
# Must run as root (debootstrap/mount/chroot need it) on a Debian/Ubuntu
# Linux host with debootstrap installed.
#
# If ROOTFS_IMG already exists, this defaults to a fast path: loop-mount the
# existing image and rsync WORKLOAD_SCRIPT onto it (regenerating run.sh /
# workload.service) instead of redoing debootstrap/apt/pip/YCSB-load from
# scratch. That's the expensive ~minutes-to-tens-of-minutes part and it
# doesn't depend on WORKLOAD_SCRIPT at all, so re-running it on every
# workload-script edit was pure waste. Set FORCE_REBUILD=1 to get the old
# full-rebuild behavior (needed after changing DISTRO/ARCH/packages/YCSB_*,
# or if ROOTFS_IMG is missing/corrupt).
set -euo pipefail

ROOTFS_IMG=${ROOTFS_IMG:?ROOTFS_IMG not set}
ROOTFS_SIZE=${ROOTFS_SIZE:-3G}
DISTRO=${DISTRO:-bookworm}
ARCH=${ARCH:-amd64}
MIRROR=${MIRROR:-http://deb.debian.org/debian}
WORKLOAD_SCRIPT=${WORKLOAD_SCRIPT:?WORKLOAD_SCRIPT not set}
KERNEL_OUT=${KERNEL_OUT:?KERNEL_OUT not set}
INITRD_OUT=${INITRD_OUT:?INITRD_OUT not set}
FORCE_REBUILD=${FORCE_REBUILD:-0}

# Optional: bake a real YCSB (Java) benchmark + a pre-loaded SQLite dataset
# in under /opt, so kvm/guest/workloads/ycsb_bench.py has something real to
# run reads against at boot. See the Makefile's GUEST_YCSB_* vars for how
# these are set; the load phase (writes) happens here, at build time,
# directly against $ROOTFS_IMG -- never at boot, where writes would land on
# the guest's volatile tmpfs and vanish instead of exercising nbd_server.
YCSB_ENABLE=${YCSB_ENABLE:-0}
YCSB_VERSION=${YCSB_VERSION:-0.17.0}
SQLITE_JDBC_VERSION=${SQLITE_JDBC_VERSION:-3.46.1.3}
YCSB_RECORDS=${YCSB_RECORDS:-1000000}
YCSB_FIELD_COUNT=${YCSB_FIELD_COUNT:-10}
YCSB_FIELD_LENGTH=${YCSB_FIELD_LENGTH:-100}
YCSB_WORKLOAD=${YCSB_WORKLOAD:-workloadc}
YCSB_OPERATIONS=${YCSB_OPERATIONS:-200000}
YCSB_DISTRIBUTION=${YCSB_DISTRIBUTION:-zipfian}
YCSB_THREADS=${YCSB_THREADS:-4}

# Optional: bake a disk-based GUPS RandomAccess table under /opt, so
# kvm/guest/workloads/gups_bench.py has something real to issue random
# reads against at boot. See the Makefile's GUEST_GUPS_* vars for how these
# are set; the table write (like YCSB's load phase) happens here, at build
# time, directly against $ROOTFS_IMG -- never at boot, where writes would
# land on the guest's volatile tmpfs and vanish instead of exercising
# nbd_server.
GUPS_ENABLE=${GUPS_ENABLE:-0}
GUPS_TABLE_MB=${GUPS_TABLE_MB:-6144}
GUPS_UPDATES=${GUPS_UPDATES:-2000000}
GUPS_BLOCK_SIZE=${GUPS_BLOCK_SIZE:-4096}

if [[ $EUID -ne 0 ]]; then
    echo "build_image.sh must run as root (debootstrap/mount/chroot need it)" >&2
    exit 1
fi

if [[ -f "$ROOTFS_IMG" && "$FORCE_REBUILD" != "1" ]]; then
    SYNC_ONLY=1
else
    SYNC_ONLY=0
fi

REQUIRED_TOOLS=(losetup chroot rsync)
[[ "$SYNC_ONLY" == "1" ]] || REQUIRED_TOOLS+=(debootstrap mkfs.ext4)
for tool in "${REQUIRED_TOOLS[@]}"; do
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

if [[ "$SYNC_ONLY" == "1" ]]; then
    echo "build_image.sh: $ROOTFS_IMG already exists -- syncing $WORKLOAD_SCRIPT onto it instead of rebuilding (set FORCE_REBUILD=1 for a full rebuild)"
else
    truncate -s "$ROOTFS_SIZE" "$ROOTFS_IMG"
    mkfs.ext4 -F -q "$ROOTFS_IMG"
fi

LOOP=$(losetup -f --show "$ROOTFS_IMG")
mount "$LOOP" "$MNT"

if [[ "$SYNC_ONLY" != "1" ]]; then

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

if [[ "$YCSB_ENABLE" == "1" ]]; then
    chroot "$MNT" env DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        openjdk-17-jre-headless sqlite3 curl

    chroot "$MNT" mkdir -p /opt/ycsb
    chroot "$MNT" curl -fsSL -o /tmp/ycsb.tar.gz \
        "https://github.com/brianfrankcooper/YCSB/releases/download/${YCSB_VERSION}/ycsb-${YCSB_VERSION}.tar.gz"
    chroot "$MNT" tar -xzf /tmp/ycsb.tar.gz -C /opt/ycsb --strip-components=1
    chroot "$MNT" rm /tmp/ycsb.tar.gz

    # Generic JDBC binding + SQLite driver -- YCSB has no bundled sqlite
    # binding, but `db=jdbc` works against any JDBC driver on its classpath.
    chroot "$MNT" mkdir -p /opt/ycsb/jdbc-binding/lib
    chroot "$MNT" curl -fsSL \
        -o "/opt/ycsb/jdbc-binding/lib/sqlite-jdbc-${SQLITE_JDBC_VERSION}.jar" \
        "https://repo1.maven.org/maven2/org/xerial/sqlite-jdbc/${SQLITE_JDBC_VERSION}/sqlite-jdbc-${SQLITE_JDBC_VERSION}.jar"

    YCSB_DB=/opt/workload/ycsb.db
    cols=""
    for ((i = 0; i < YCSB_FIELD_COUNT; i++)); do
        cols+="FIELD${i} TEXT, "
    done
    chroot "$MNT" sqlite3 "$YCSB_DB" \
        "CREATE TABLE USERTABLE (YCSB_KEY VARCHAR(255) PRIMARY KEY, ${cols%, });"

    # Load phase: writes the actual dataset, once, directly into $ROOTFS_IMG
    # via this chroot -- a real disk write, unlike anything a *running*
    # guest does. Single-threaded: SQLite serializes writers, so parallel
    # `ycsb load` threads would just contend on the same file lock.
    #
    # Invokes java directly instead of going through the release tarball's
    # bin/ycsb launcher: that launcher is a thin Python 2 script (resolves
    # "jdbc" -> a Java class via a hardcoded dict, builds this same
    # classpath, execs java -load/-t) that doesn't run under python3 at all
    # (Debian bookworm ships no python2), and porting it wasn't worth it for
    # what amounts to four lines of classpath logic. See ycsb_bench.py's
    # `run` invocation below for the matching boot-time command.
    YCSB_CP="/opt/ycsb/jdbc-binding/conf:/opt/ycsb/conf:/opt/ycsb/lib/*:/opt/ycsb/jdbc-binding/lib/*"
    chroot "$MNT" bash -c "cd /opt/ycsb && java -cp '$YCSB_CP' site.ycsb.Client -load \
        -db site.ycsb.db.JdbcDBClient \
        -P workloads/${YCSB_WORKLOAD} \
        -p db.driver=org.sqlite.JDBC -p db.url=jdbc:sqlite:${YCSB_DB} \
        -p recordcount=${YCSB_RECORDS} -p fieldcount=${YCSB_FIELD_COUNT} \
        -p fieldlength=${YCSB_FIELD_LENGTH} -threads 1"

    cat > "$MNT/opt/workload/ycsb_config.json" <<EOF
{
  "ycsb_home": "/opt/ycsb",
  "db_path": "${YCSB_DB}",
  "workload": "${YCSB_WORKLOAD}",
  "recordcount": ${YCSB_RECORDS},
  "operationcount": ${YCSB_OPERATIONS},
  "fieldcount": ${YCSB_FIELD_COUNT},
  "fieldlength": ${YCSB_FIELD_LENGTH},
  "requestdistribution": "${YCSB_DISTRIBUTION}",
  "threads": ${YCSB_THREADS}
}
EOF
fi

if [[ "$GUPS_ENABLE" == "1" ]]; then
    # Written with real zero bytes rather than left as a sparse hole: a
    # hole would let the *guest's own* ext4 resolve reads as all-zero
    # in-kernel without ever issuing a block I/O request, so nbd_server
    # (and the cache/prefetch policy under test) would never see the
    # access at all -- same reasoning as YCSB's real SQLite rows above.
    GUPS_TABLE=/opt/workload/gups_table.bin
    chroot "$MNT" dd if=/dev/zero of="$GUPS_TABLE" bs=1M count="$GUPS_TABLE_MB" status=none

    cat > "$MNT/opt/workload/gups_config.json" <<EOF
{
  "table_path": "${GUPS_TABLE}",
  "table_size_bytes": $((GUPS_TABLE_MB * 1024 * 1024)),
  "updates": ${GUPS_UPDATES},
  "block_size": ${GUPS_BLOCK_SIZE}
}
EOF
fi

fi # SYNC_ONLY

mkdir -p "$MNT/opt/workload"
rsync -a "$WORKLOAD_SCRIPT" "$MNT/opt/workload/run.py"

# Powers the guest off once the workload exits (success or failure) so that
# a host-side `kvm/launch.py --wait` can detect completion by watching for
# the QEMU process to exit, rather than polling the serial log's output.
# poweroff always runs regardless of the workload's exit status -- a guest
# that doesn't power off on failure would just trade one hang for another --
# but the WORKLOAD_RESULT line (StandardOutput=journal+console below sends
# it to the serial console for free) lets the host tell a crashed run from a
# successful one after the fact, since QEMU exiting looks identical either
# way (see kvm/launch.py's check_workload_result()).
cat > "$MNT/opt/workload/run.sh" <<'EOF'
#!/bin/sh
python3 /opt/workload/run.py
status=$?
if [ "$status" -eq 0 ]; then
    echo "WORKLOAD_RESULT: PASS"
else
    echo "WORKLOAD_RESULT: FAIL exit=$status"
fi
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

# `systemctl enable` inside a bare chroot (no live systemd/D-Bus) falls back
# to a filesystem-only "offline enablement" heuristic that isn't guaranteed
# to fire the same way on every systemd version/host -- and the failure mode
# when it doesn't is a guest that boots perfectly fine and then just sits at
# a login prompt forever, since nothing ever runs the workload or calls
# poweroff. That's expensive to debug (looks identical to a real hang) and
# cheap to rule out here, so enable it ourselves with the one symlink
# `[Install] WantedBy=multi-user.target` means, then verify it -- and the
# files it points at -- actually exist before calling the image done.
mkdir -p "$MNT/etc/systemd/system/multi-user.target.wants"
ln -sf ../workload.service "$MNT/etc/systemd/system/multi-user.target.wants/workload.service"

if [[ ! -L "$MNT/etc/systemd/system/multi-user.target.wants/workload.service" ]]; then
    echo "build_image.sh: FAILED to enable workload.service -- the guest would boot fine and never run its workload" >&2
    exit 1
fi
if [[ ! -x "$MNT/opt/workload/run.sh" ]]; then
    echo "build_image.sh: FAILED -- /opt/workload/run.sh is missing or not executable" >&2
    exit 1
fi
if [[ ! -f "$MNT/opt/workload/run.py" ]]; then
    echo "build_image.sh: FAILED -- /opt/workload/run.py (from WORKLOAD_SCRIPT=$WORKLOAD_SCRIPT) is missing" >&2
    exit 1
fi

if [[ "$SYNC_ONLY" != "1" ]]; then

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

fi # SYNC_ONLY

if [[ "$SYNC_ONLY" == "1" ]]; then
    echo "guest image synced: $ROOTFS_IMG (workload only -- kernel/initrd/packages/YCSB dataset untouched; set FORCE_REBUILD=1 for a full rebuild)"
else
    echo "guest image built: $ROOTFS_IMG"
fi
echo "kernel:  $KERNEL_OUT"
echo "initrd:  $INITRD_OUT"
echo "workload: $WORKLOAD_SCRIPT -> /opt/workload/run.py (runs once per boot via workload.service)"
if [[ "$YCSB_ENABLE" == "1" ]]; then
    echo "ycsb: ${YCSB_RECORDS} records loaded into /opt/workload/ycsb.db, ${YCSB_WORKLOAD} read at boot (see /opt/workload/ycsb_config.json)"
fi
if [[ "$GUPS_ENABLE" == "1" ]]; then
    echo "gups: ${GUPS_TABLE_MB}MB table baked at /opt/workload/gups_table.bin, ${GUPS_UPDATES} random ${GUPS_BLOCK_SIZE}-byte reads at boot (see /opt/workload/gups_config.json)"
fi
