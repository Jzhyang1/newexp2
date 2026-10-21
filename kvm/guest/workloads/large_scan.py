"""Sequentially scan more data than the VM can keep in its page cache.

The default VM has 4 GiB of RAM, so the default 8 GiB scan forces earlier
pages out while reads continue through the guest's raw block device. The
scan is intentionally read-only; writes from a running guest are discarded
by nbd_server anyway.
"""
import argparse
import os
import time


DEFAULT_SCAN_BYTES = 8 * 1024 * 1024 * 1024
DEFAULT_CHUNK_BYTES = 1 * 1024 * 1024


def parse_size(value):
    """Parse a positive byte count with an optional KiB/MiB/GiB suffix."""
    text = value.strip().upper()
    multiplier = 1
    multipliers = {"K": 1024, "KB": 1024, "M": 1024**2, "MB": 1024**2,
                   "G": 1024**3, "GB": 1024**3}
    for suffix, suffix_multiplier in multipliers.items():
        if text.endswith(suffix):
            text = text[:-len(suffix)].strip()
            multiplier = suffix_multiplier
            break
    try:
        size = int(text) * multiplier
    except ValueError as error:
        raise argparse.ArgumentTypeError("size must be an integer") from error
    if size <= 0:
        raise argparse.ArgumentTypeError("size must be positive")
    return size


def kernel_arg(name):
    prefix = f"{name}="
    try:
        cmdline = open("/proc/cmdline").read().split()
    except OSError:
        return None
    for argument in cmdline:
        if argument.startswith(prefix):
            return argument[len(prefix):]
    return None


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--device", default="/dev/vda")
    parser.add_argument("--scan-bytes", type=parse_size)
    parser.add_argument("--chunk-bytes", type=parse_size)
    args = parser.parse_args()

    if args.scan_bytes is None:
        args.scan_bytes = parse_size(kernel_arg("scan_bytes") or str(DEFAULT_SCAN_BYTES))
    if args.chunk_bytes is None:
        args.chunk_bytes = parse_size(kernel_arg("chunk_bytes") or str(DEFAULT_CHUNK_BYTES))

    device_size = os.stat(args.device).st_size
    if device_size and args.scan_bytes > device_size:
        raise RuntimeError(
            f"requested scan ({args.scan_bytes} bytes) exceeds "
            f"device size ({device_size} bytes)"
        )

    chunks = (args.scan_bytes + args.chunk_bytes - 1) // args.chunk_bytes
    print(
        f"LARGE_SCAN starting: {args.scan_bytes} bytes in {chunks} "
        f"{args.chunk_bytes}-byte reads from {args.device}",
        flush=True,
    )

    start = time.time()
    total = 0
    with open(args.device, "rb", buffering=0) as device:
        for offset in range(0, args.scan_bytes, args.chunk_bytes):
            read_size = min(args.chunk_bytes, args.scan_bytes - offset)
            data = os.pread(device.fileno(), read_size, offset)
            if len(data) != read_size:
                raise RuntimeError(
                    f"short read at offset={offset}: got {len(data)} bytes"
                )
            total += len(data)

    elapsed = time.time() - start
    print(
        f"LARGE_SCAN complete: {total} bytes in {elapsed:.2f}s "
        f"({total / elapsed / 1024**2:.1f} MiB/s)",
        flush=True,
    )


if __name__ == "__main__":
    main()