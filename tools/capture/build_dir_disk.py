#!/usr/bin/env python3
"""Build an ext4 data disk from a directory (or an empty one), no root needed, for `run_capture.py --workload cmd`.

  build_dir_disk.py --out disk/data/linux_data.img --src-tar disk/data/src/linux-6.6.tar.xz
  build_dir_disk.py --out disk/data/rocks_data.img --empty-gib 24

Same conventions as build_ycsb_disk.py: `mke2fs -d` populates the file system from a staging directory (files are laid
out in the order mke2fs walks the tree, as an untar onto a fresh fs would roughly do), a raw tail follows the file
system for the guest's O_DIRECT clock-sync probes, and `<out>.meta.json` records fs_bytes / tail_offset.  `--free-gib`
leaves room for a guest that writes (writes go to nbd_server's overlay, but ext4 still needs free blocks).
"""
import argparse, json, os, shutil, subprocess, tarfile, time


def du(path):
    return sum(os.path.getsize(os.path.join(d, f)) for d, _, fs in os.walk(path) for f in fs if not os.path.islink(os.path.join(d, f)))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True)
    ap.add_argument("--src-tar", default="", help="tarball to extract into the file system (data only, nothing is executed)")
    ap.add_argument("--src-dir", default="")
    ap.add_argument("--empty-gib", type=float, default=0, help="no content: an empty fs of this size")
    ap.add_argument("--free-gib", type=float, default=1.0)
    ap.add_argument("--tail-mib", type=int, default=16)
    ap.add_argument("--label", default="data")
    a = ap.parse_args()
    t0 = time.time()
    stage = os.path.abspath(a.out) + ".stage"
    shutil.rmtree(stage, ignore_errors=True)
    os.makedirs(stage)
    if a.src_tar:
        with tarfile.open(a.src_tar) as tf:
            tf.extractall(stage, filter="data")
    elif a.src_dir:
        shutil.copytree(a.src_dir, os.path.join(stage, os.path.basename(a.src_dir.rstrip("/"))), symlinks=True)
    content = du(stage)
    nfiles = sum(len(fs) for _, _, fs in os.walk(stage))
    fs_bytes = int(max(content * 1.15 + a.free_gib * 2**30 + 64 * 2**20, a.empty_gib * 2**30))
    fs_bytes = (fs_bytes + 2**20 - 1) // 2**20 * 2**20
    if os.path.exists(a.out):
        os.remove(a.out)
    subprocess.check_call(["mke2fs", "-q", "-t", "ext4", "-F", "-L", a.label, "-b", "4096", "-N", str(max(65536, nfiles * 2)),
                           "-O", "^has_journal,extent,^resize_inode", "-d", stage, a.out, f"{fs_bytes // 1024}k"])
    with open(a.out, "r+b") as f:
        f.truncate(fs_bytes + a.tail_mib * 2**20)
    meta = dict(workload="cmd", source=a.src_tar or a.src_dir or "empty", content_bytes=content, files=nfiles,
                fs_bytes=fs_bytes, tail_offset=fs_bytes, tail_bytes=a.tail_mib * 2**20)
    json.dump(meta, open(a.out + ".meta.json", "w"))
    shutil.rmtree(stage)
    print(f"{a.out}: {nfiles} files, {content / 2**30:.2f} GiB content, fs {fs_bytes / 2**30:.2f} GiB ({time.time() - t0:.0f}s)")


if __name__ == "__main__":
    main()
