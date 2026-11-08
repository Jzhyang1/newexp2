"""Guest-side hint shim: tell the host what this workload is about to read, in-band, over a spare block device.

The host exposes a second NBD disk (/dev/vdc, `hint_port` in nbd.yaml).  A hint is a 512-byte O_DIRECT read at byte
offset 512 * payload, payload = (page4k << 8) | (lenclass << 4) | aux  (see HintReader in disk/nbd.cpp): page4k = the 4 KiB
block of the data disk, lenclass indexes LEN (4 KiB units), aux = 0..14 residency or 15 = unknown.  Reads return zeros.
"""
import ctypes
import mmap
import os

LEN = [1, 2, 4, 6, 8, 12, 16, 24, 32, 48, 64, 96, 128, 192, 256, 384]


class Hinter:
    def __init__(self, dev="/dev/vdc"):
        self.fd = os.open(dev, os.O_RDONLY | os.O_DIRECT)
        self.buf = mmap.mmap(-1, 4096)
        self.sent = 0

    def hint(self, byte_off, nbytes, aux=15):
        """Hint [byte_off, byte_off + nbytes) of the data disk (as several hints when it is longer than 1.5 MiB)."""
        first = byte_off // 4096
        units = (byte_off + nbytes + 4095) // 4096 - first
        while units > 0:
            cls = max((i for i, u in enumerate(LEN) if u <= units), default=0)
            if LEN[cls] < units and units < LEN[-1] and cls + 1 < len(LEN) and units - LEN[cls] < LEN[cls] // 2 + 1 and LEN[cls + 1] >= units:
                cls += 1                                  # a slightly larger class covers the tail in one hint
            payload = (first << 8) | (cls << 4) | aux
            os.preadv(self.fd, [memoryview(self.buf)[:512]], payload * 512)
            self.sent += 1
            first += LEN[cls]
            units -= LEN[cls]


class Mincore:
    """Fraction of a file range that is resident in the guest page cache (the same pages faiss mmaps), via mincore(2)."""
    def __init__(self, path):
        self.fd = os.open(path, os.O_RDONLY)
        self.size = os.fstat(self.fd).st_size
        self.libc = ctypes.CDLL("libc.so.6", use_errno=True)
        self.libc.mmap.restype = ctypes.c_void_p
        self.libc.mmap.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_long]
        self.libc.mincore.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p]
        self.base = self.libc.mmap(None, self.size, 1, 1, self.fd, 0)        # PROT_READ, MAP_SHARED: the page cache faiss uses

    def resident_fraction(self, off, nbytes):
        a = off // 4096 * 4096
        n = (off + nbytes - a + 4095) // 4096
        if n <= 0:
            return None
        vec = (ctypes.c_ubyte * n)()
        if self.libc.mincore(ctypes.c_void_p(self.base + a), n * 4096, vec) != 0:
            return None
        return sum(v & 1 for v in vec) / n
