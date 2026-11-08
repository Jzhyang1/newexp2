#!/usr/bin/env python3
"""Export the side information `ivfgeo` needs from a FAISS data image: centroids (index.faiss) and the byte extent of every inverted list.
Needs faiss on the host (pip install faiss-cpu) and debugfs. usage: geo_export.py [data.img] [out.bin]"""
import json, os, struct, subprocess, sys, tempfile
import numpy as np
import faiss
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))) + "/capture")
import capture_lib
R = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))) + "/"
img = sys.argv[1] if len(sys.argv) > 1 else R + "disk/data/faiss_data.img"
out = sys.argv[2] if len(sys.argv) > 2 else R + "disk/data/faiss_geo.bin"
with tempfile.TemporaryDirectory() as d:
    subprocess.run(["debugfs", "-R", f"dump /index.faiss {d}/index.faiss", img], check=True, capture_output=True)
    idx = faiss.read_index(d + "/index.faiss", faiss.IO_FLAG_SKIP_IVF_DATA)
    cent = faiss.rev_swig_ptr(faiss.downcast_index(idx.quantizer).get_xb(), idx.nlist * idx.d).reshape(idx.nlist, idx.d).astype("float32")
lay = capture_lib.Layout(img, json.load(open(img + ".meta.json")))
with open(out, "wb") as f:
    f.write(struct.pack("<II", idx.nlist, idx.d)); f.write(cent.tobytes())
    f.write(np.asarray(lay.list_disk_start, dtype="<u8").tobytes()); f.write(np.asarray(lay.list_bytes, dtype="<u8").tobytes())
print(out, idx.nlist, "lists,", idx.d, "dims")
