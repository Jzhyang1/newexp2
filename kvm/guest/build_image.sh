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
# If THESIOS_ENABLE=1, also bakes a condensed public Thesios trace (filename,
# offset, length per read) under /opt/workload for thesios_bench.py to replay
# at boot, and grows ROOTFS_IMG to a sparse THESIOS_DISK_SIZE: the bench gives
# each trace file a hashed offset past the ext4 filesystem (which keeps its
# own size), so the replay spans the whole disk instead of one file's range.
#
# All writes here go straight to ROOTFS_IMG on the host filesystem via
# debootstrap/chroot -- a completely different path from nbd_server, which
# discards every write a *running* guest makes to this same file (see
# disk/sim.hpp). That's why the guest boots with systemd.volatile=state
# (root, including /opt, stays read-only from the real disk; only /var is a
# tmpfs overlay) -- NOT systemd.volatile=yes, which would tmpfs everything
# outside /usr and wipe /opt/workload on every boot.
#
# Datasets are downloaded ahead of time by the Makefile (`make thesios-download`,
# `make ycsb-download`) into THESIOS_DIR / YCSB_ASSETS_DIR; this script only
# picks the pieces it needs from those directories (the shards named by
# THESIOS_TRACE_URLS, the YCSB tarball + JDBC driver for the given versions)
# and bakes them in. Thesios shards missing from THESIOS_DIR fall back to a
# direct download; a missing YCSB asset is an error.
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
ROOTFS_SIZE=${ROOTFS_SIZE:-12G}
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
# Single-threaded by default so a fixed seed produces a reproducible I/O trace.
YCSB_THREADS=${YCSB_THREADS:-1}
YCSB_SEED=${YCSB_SEED:-20261007}
# Directory holding ycsb-<ver>.tar.gz and sqlite-jdbc-<ver>.jar (from
# `make ycsb-download`).
YCSB_ASSETS_DIR=${YCSB_ASSETS_DIR:-$(dirname "$ROOTFS_IMG")/assets/ycsb}

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
GUPS_DIRECT_IO=${GUPS_DIRECT_IO:-0}
GUPS_SEED=${GUPS_SEED:-20261007}

# Optional: bake a condensed Thesios trace into /opt/workload. The trace
# itself is downloaded on the host, not from inside the guest, so boot does
# not depend on network access. See the Makefile's GUEST_THESIOS_* vars.
THESIOS_ENABLE=${THESIOS_ENABLE:-0}
THESIOS_TRACE_URLS=${THESIOS_TRACE_URLS:-https://storage.googleapis.com/thesios-io-traces/cluster1_16TB/20240115/data-00000-of-00100}
THESIOS_MAX_REQUESTS=${THESIOS_MAX_REQUESTS:-0}
THESIOS_DEVICE=${THESIOS_DEVICE:-/dev/vda}
THESIOS_DIRECT_IO=${THESIOS_DIRECT_IO:-0}
# Sparse size the image is grown to; must stay under the host filesystem's
# max file size (16T on ext4).
THESIOS_DISK_SIZE=${THESIOS_DISK_SIZE:-8T}
THESIOS_SEED=${THESIOS_SEED:-20261007}
# Directory of raw shards named like the URL basenames (`make thesios-download`).
THESIOS_DIR=${THESIOS_DIR:-$(dirname "$ROOTFS_IMG")/assets/thesios}
IFS=',' read -ra THESIOS_URLS <<< "$THESIOS_TRACE_URLS"
THESIOS_TMP=$(mktemp)
# Host-side cache of condensed shards, so changing/adding shards (or
# rebuilding the image) never re-downloads ones already fetched.
THESIOS_CACHE=${THESIOS_CACHE:-$(dirname "$ROOTFS_IMG")/thesios_cache}

if [[ "$THESIOS_DIRECT_IO" != "0" && "$THESIOS_DIRECT_IO" != "1" ]]; then
    echo "THESIOS_DIRECT_IO must be 0 or 1" >&2
    exit 1
fi

if [[ $EUID -ne 0 ]]; then
    echo "build_image.sh must run as root (debootstrap/mount/chroot need it)" >&2
    exit 1
fi

REBUILD_ROOTFS=0
if [[ "$FORCE_REBUILD" == "1" || ! -f "$ROOTFS_IMG" ]]; then
    REBUILD_ROOTFS=1
fi

REQUIRED_TOOLS=(losetup chroot rsync mount mkfs.ext4)
[[ "$THESIOS_ENABLE" == "1" ]] && REQUIRED_TOOLS+=(dumpe2fs numfmt)
[[ "$REBUILD_ROOTFS" == "1" ]] && REQUIRED_TOOLS+=(debootstrap)
for tool in "${REQUIRED_TOOLS[@]}"; do
    command -v "$tool" >/dev/null || { echo "missing required tool: $tool" >&2; exit 1; }
done
YCSB_TARBALL="$YCSB_ASSETS_DIR/ycsb-${YCSB_VERSION}.tar.gz"
SQLITE_JDBC_JAR="$YCSB_ASSETS_DIR/sqlite-jdbc-${SQLITE_JDBC_VERSION}.jar"
if [[ "$YCSB_ENABLE" == "1" ]]; then
    for f in "$YCSB_TARBALL" "$SQLITE_JDBC_JAR"; do
        [[ -s "$f" ]] || { echo "missing YCSB asset: $f (run make ycsb-download)" >&2; exit 1; }
    done
fi
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
    [[ -n "$THESIOS_TMP" ]] && rm -f "$THESIOS_TMP"
    rmdir "$MNT"
}
trap cleanup EXIT

mkdir -p "$(dirname "$ROOTFS_IMG")" "$(dirname "$KERNEL_OUT")" "$(dirname "$INITRD_OUT")"

# 3. Heavy-duty work: initialize the rootfs and provision missing datasets.
if [[ "$REBUILD_ROOTFS" == "1" ]]; then
    echo "build_image.sh: building a fresh rootfs at $ROOTFS_IMG"
    truncate -s "$ROOTFS_SIZE" "$ROOTFS_IMG"
    mkfs.ext4 -F -q "$ROOTFS_IMG"
else
    echo "build_image.sh: reusing existing rootfs $ROOTFS_IMG"
fi

# Thesios: the filesystem keeps its size and the rest of the (sparse) image is
# the space thesios_bench.py lays trace files out in. Done before
# mounting so a reused image gets (re)sized too; growing a file under an ext4
# filesystem is harmless, and the guest sees the larger /dev/vda.
THESIOS_FS_BYTES=0
if [[ "$THESIOS_ENABLE" == "1" ]]; then
    THESIOS_FS_BYTES=$(dumpe2fs -h "$ROOTFS_IMG" 2>/dev/null \
        | awk -F: '/^Block count/ {c=$2} /^Block size/ {b=$2} END {print c * b}')
    THESIOS_DISK_BYTES=$(numfmt --from=iec "$THESIOS_DISK_SIZE")
    if (( THESIOS_FS_BYTES <= 0 || THESIOS_DISK_BYTES <= THESIOS_FS_BYTES )); then
        echo "THESIOS_DISK_SIZE=$THESIOS_DISK_SIZE must exceed the ${THESIOS_FS_BYTES}-byte filesystem" >&2
        exit 1
    fi
    truncate -s "$THESIOS_DISK_BYTES" "$ROOTFS_IMG"
fi

LOOP=$(losetup -f --show "$ROOTFS_IMG")
mount "$LOOP" "$MNT"

BASE_MISSING=0
[[ -x "$MNT/usr/bin/python3" && -x "$MNT/usr/bin/systemctl" ]] || BASE_MISSING=1
[[ "$REBUILD_ROOTFS" == "1" ]] && BASE_MISSING=1

# Each dataset records the parameters it was built with in a stamp file, so a
# changed parameter (record count, table size, shard list...) regenerates just
# that dataset while an unchanged one is reused as-is.
STAMP_DIR="$MNT/opt/workload/.stamps"
write_stamp() { mkdir -p "$STAMP_DIR"; printf '%s' "$2" > "$STAMP_DIR/$1"; }
# An image built before stamps existed has data but no stamp: adopt it under
# the current parameters rather than regenerating GBs of data.
stamp_ok() {
    [[ -f "$STAMP_DIR/$1" ]] || { write_stamp "$1" "$2"; return 0; }
    [[ "$(cat "$STAMP_DIR/$1")" == "$2" ]]
}

YCSB_STAMP="$YCSB_VERSION $SQLITE_JDBC_VERSION $YCSB_RECORDS $YCSB_FIELD_COUNT $YCSB_FIELD_LENGTH $YCSB_WORKLOAD $YCSB_SEED"
GUPS_STAMP="$GUPS_TABLE_MB"
# "v2": the trace carries a filename column (older images baked offset,length only).
THESIOS_STAMP=$(printf 'v2\n%s\n' "${THESIOS_URLS[@]}" | sha256sum | cut -d' ' -f1)

YCSB_MISSING=0
if [[ "$YCSB_ENABLE" == "1" ]]; then
    [[ -x "$MNT/usr/bin/java" && -x "$MNT/usr/bin/sqlite3" && -d "$MNT/opt/ycsb/lib" && -s "$MNT/opt/workload/ycsb.db" ]] \
        && stamp_ok ycsb "$YCSB_STAMP" || YCSB_MISSING=1
fi

GUPS_MISSING=0
if [[ "$GUPS_ENABLE" == "1" ]]; then
    [[ -f "$MNT/opt/workload/gups_table.bin" ]] \
        && [[ "$(stat -c %s "$MNT/opt/workload/gups_table.bin")" == "$((GUPS_TABLE_MB * 1024 * 1024))" ]] \
        && stamp_ok gups "$GUPS_STAMP" || GUPS_MISSING=1
fi

THESIOS_MISSING=0
if [[ "$THESIOS_ENABLE" == "1" ]]; then
    [[ -s "$MNT/opt/workload/thesios_trace.csv" ]] && stamp_ok thesios "$THESIOS_STAMP" || THESIOS_MISSING=1
fi

# Anything that runs inside the chroot (apt, curl, java, dd if=/dev/zero)
# needs DNS and /dev,/proc,/sys -- set up once here instead of only in the
# base-install branch, which a reused image skips.
if [[ "$BASE_MISSING" == "1" || "$YCSB_MISSING" == "1" || "$GUPS_MISSING" == "1" ]]; then
    if [[ "$REBUILD_ROOTFS" == "1" ]]; then
        debootstrap --arch="$ARCH" "$DISTRO" "$MNT" "$MIRROR"
    fi
    cp /etc/resolv.conf "$MNT/etc/resolv.conf"
    mount --bind /dev "$MNT/dev"
    mount -t proc proc "$MNT/proc"
    mount -t sysfs sysfs "$MNT/sys"
fi

if [[ "$BASE_MISSING" == "1" ]]; then

chroot "$MNT" env DEBIAN_FRONTEND=noninteractive apt-get update
chroot "$MNT" env DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
    linux-image-"$ARCH" systemd-sysv ca-certificates python3 python3-pip python3-numpy
chroot "$MNT" pip3 install --break-system-packages --no-cache-dir faiss-cpu

mkdir -p "$MNT/opt/workload"

fi

YCSB_DB=/opt/workload/ycsb.db
if [[ "$YCSB_MISSING" == "1" ]]; then
    chroot "$MNT" env DEBIAN_FRONTEND=noninteractive apt-get update
    chroot "$MNT" env DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        openjdk-17-jre-headless sqlite3

    chroot "$MNT" rm -rf /opt/ycsb "$YCSB_DB"
    chroot "$MNT" mkdir -p /opt/ycsb
    tar -xzf "$YCSB_TARBALL" -C "$MNT/opt/ycsb" --strip-components=1

    # Generic JDBC binding + SQLite driver -- YCSB has no bundled sqlite
    # binding, but `db=jdbc` works against any JDBC driver on its classpath.
    mkdir -p "$MNT/opt/ycsb/jdbc-binding/lib"
    cp "$SQLITE_JDBC_JAR" "$MNT/opt/ycsb/jdbc-binding/lib/"

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
        -p db.driver=org.sqlite.JDBC \
        -p 'db.url=jdbc:sqlite:${YCSB_DB}?journal_mode=OFF&synchronous=OFF' \
        -p jdbc.autocommit=false -p jdbc.batchupdateapi=true -p db.batchsize=1000 \
        -p recordcount=${YCSB_RECORDS} -p fieldcount=${YCSB_FIELD_COUNT} \
        -p fieldlength=${YCSB_FIELD_LENGTH} -p seed=${YCSB_SEED} -threads 1"
    write_stamp ycsb "$YCSB_STAMP"
fi

if [[ "$GUPS_MISSING" == "1" ]]; then
    # Written with real zero bytes rather than left as a sparse hole: a
    # hole would let the *guest's own* ext4 resolve reads as all-zero
    # in-kernel without ever issuing a block I/O request, so nbd_server
    # (and the cache/prefetch policy under test) would never see the
    # access at all -- same reasoning as YCSB's real SQLite rows above.
    GUPS_TABLE=/opt/workload/gups_table.bin
    chroot "$MNT" rm -f "$GUPS_TABLE"
    chroot "$MNT" dd if=/dev/zero of="$GUPS_TABLE" bs=1M count="$GUPS_TABLE_MB" status=none
    write_stamp gups "$GUPS_STAMP"
fi

if [[ "$THESIOS_MISSING" == "1" ]]; then
    PREPARE_ARGS=()
    for url in "${THESIOS_URLS[@]}"; do
        PREPARE_ARGS+=(--url "$url")
    done
    python3 kvm/guest/prepare_thesios.py --cache-dir "$THESIOS_CACHE" --trace-dir "$THESIOS_DIR" \
        --output "$THESIOS_TMP" "${PREPARE_ARGS[@]}"
    cp "$THESIOS_TMP" "$MNT/opt/workload/thesios_trace.csv"
    write_stamp thesios "$THESIOS_STAMP"
fi

# 2. Per-run synchronization: workload code, config files, and service wiring.
mkdir -p "$MNT/opt/workload"
if [[ "$GUPS_ENABLE" == "1" ]]; then
        cat > "$MNT/opt/workload/gups_config.json" <<EOF
{
    "table_path": "/opt/workload/gups_table.bin",
    "table_size_bytes": $((GUPS_TABLE_MB * 1024 * 1024)),
    "updates": ${GUPS_UPDATES},
    "block_size": ${GUPS_BLOCK_SIZE},
    "direct_io": ${GUPS_DIRECT_IO},
    "seed": ${GUPS_SEED}
}
EOF
fi
if [[ "$THESIOS_ENABLE" == "1" ]]; then
    THESIOS_TRACE=/opt/workload/thesios_trace.csv
    cat > "$MNT/opt/workload/thesios_config.json" <<EOF
{
    "trace_path": "${THESIOS_TRACE}",
    "device_path": "${THESIOS_DEVICE}",
    "max_requests": ${THESIOS_MAX_REQUESTS},
    "direct_io": ${THESIOS_DIRECT_IO},
    "region_start_bytes": ${THESIOS_FS_BYTES},
    "seed": ${THESIOS_SEED}
}
EOF
fi
rsync -a "$WORKLOAD_SCRIPT" "$MNT/opt/workload/run.py"

# Powers the guest off once the workload exits (success or failure) so that
if [[ "$YCSB_ENABLE" == "1" ]]; then
    cat > "$MNT/opt/workload/ycsb_config.json" <<EOF
{
  "ycsb_home": "/opt/ycsb",
  "db_path": "/opt/workload/ycsb.db",
  "workload": "${YCSB_WORKLOAD}",
  "recordcount": ${YCSB_RECORDS},
  "operationcount": ${YCSB_OPERATIONS},
  "fieldcount": ${YCSB_FIELD_COUNT},
  "fieldlength": ${YCSB_FIELD_LENGTH},
  "requestdistribution": "${YCSB_DISTRIBUTION}",
    "threads": ${YCSB_THREADS},
    "seed": ${YCSB_SEED}
}
EOF
fi
# a host-side `kvm/launch.py --wait` can detect completion by watching for
# the QEMU process to exit, rather than polling the serial log's output.
# poweroff always runs regardless of the workload's exit status -- a guest
# that doesn't power off on failure would just trade one hang for another --
# but the WORKLOAD_RESULT line (StandardOutput=journal+console below sends
# it to the serial console for free) lets the host tell a crashed run from a
# successful one after the fact, since QEMU exiting looks identical either
# way (see kvm/launch.py's check_workload_result()).
#
# The workload.info file is a coarse approximation of workload-vs-kernel
# time: it's the wall time of run.py alone, so boot/shutdown time
# (kernel-incurred) is whatever the host sees beyond it. It's written into the
# 9p directory kvm/launch.py shares with each VM (mount_tag=results) and read
# by nbd_server into nbd_results.csv (see ReadWorkloadInfo in disk/nbd.cpp).
# /run is used as the mount point because root is read-only. If the mount
# fails (no share configured) the line still goes to the console, and
# nothing else depends on it.
basename "$WORKLOAD_SCRIPT" .py > "$MNT/opt/workload/workload_name"
cat > "$MNT/opt/workload/run.sh" <<'EOF'
#!/bin/sh
start=$(date +%s.%N)
python3 /opt/workload/run.py
status=$?
end=$(date +%s.%N)
if [ "$status" -eq 0 ]; then
    echo "WORKLOAD_RESULT: PASS"
else
    echo "WORKLOAD_RESULT: FAIL exit=$status"
fi
runtime=$(awk -v s="$start" -v e="$end" 'BEGIN { printf "%.3f", e - s }')
info="name=$(cat /opt/workload/workload_name) runtime_seconds=$runtime"
echo "WORKLOAD_INFO: $info"
mkdir -p /run/results
modprobe 9pnet_virtio 2>/dev/null; modprobe 9p 2>/dev/null
if mount -t 9p -o trans=virtio results /run/results 2>/dev/null; then
    echo "$info" > /run/results/workload.info
    sync
    umount /run/results
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
if [[ "$THESIOS_ENABLE" == "1" && ! -f "$MNT/opt/workload/thesios_config.json" ]]; then
    echo "build_image.sh: FAILED -- Thesios configuration is missing" >&2
    exit 1
fi
if [[ "$THESIOS_ENABLE" == "1" && ! -s "$MNT/opt/workload/thesios_trace.csv" ]]; then
    echo "build_image.sh: FAILED -- downloaded Thesios trace is empty" >&2
    exit 1
fi

# There are currently no tasks exclusive to FORCE_REBUILD. Recreating the
# rootfs above naturally reruns all heavy provisioning; these boot artifacts
# are needed whenever the base image was newly provisioned or repaired.
if [[ "$BASE_MISSING" == "1" ]]; then

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

fi

if [[ "$BASE_MISSING" == "1" || ! -s "$KERNEL_OUT" || ! -s "$INITRD_OUT" ]]; then

KVER=$(chroot "$MNT" bash -c "ls /boot | grep '^vmlinuz-' | sort -V | tail -1")
INITRD_NAME=$(chroot "$MNT" bash -c "ls /boot | grep '^initrd.img-' | sort -V | tail -1")
cp "$MNT/boot/$KVER" "$KERNEL_OUT"
cp "$MNT/boot/$INITRD_NAME" "$INITRD_OUT"

fi

if [[ "$REBUILD_ROOTFS" == "0" ]]; then
    echo "guest image updated: $ROOTFS_IMG (workload/configs synced; missing datasets repaired as needed)"
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
if [[ "$THESIOS_ENABLE" == "1" ]]; then
    echo "thesios: condensed ${#THESIOS_URLS[@]} shard(s) baked at /opt/workload/thesios_trace.csv, max_requests=${THESIOS_MAX_REQUESTS} (0=all), image grown to sparse ${THESIOS_DISK_SIZE} (files laid out past byte ${THESIOS_FS_BYTES})"
fi
