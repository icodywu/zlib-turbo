"""Seed corpus for zdiff_fuzz.c from a directory of files: for each file (its first 20,000 bytes), zlib, raw and
gzip streams at levels 1, 6 and 9 with windows of 512 B, 4 KiB and 32 KiB, with random piece sizes; plus one
case per file that has the fuzzer deflate the data itself.

usage: python3 tests/fuzz/make_seeds.py INPUT_DIR CORPUS_DIR
"""
import os
import random
import sys
import zlib

src, out = sys.argv[1], sys.argv[2]
os.makedirs(out, exist_ok=True)
random.seed(1)
n = 0
for f in sorted(os.listdir(src)):
    path = os.path.join(src, f)
    if not os.path.isfile(path):
        continue
    d = open(path, "rb").read()[:20000]
    for level in (1, 6, 9):
        for wsel, wbits in ((0, 15), (1, -15), (2, 31), (3, 9), (5, 12)):
            co = zlib.compressobj(level, zlib.DEFLATED, wbits)
            stream = co.compress(d) + co.flush()
            sizes = bytes(random.randrange(256) for _ in range(5))
            osizes = bytes(random.randrange(256) for _ in range(5))
            with open(os.path.join(out, "s%05d" % n), "wb") as g:
                g.write(bytes([0, wsel, 5]) + sizes + bytes([5]) + osizes + stream)
            n += 1
    with open(os.path.join(out, "s%05d" % n), "wb") as g:
        g.write(bytes([0x80 | 6, 0, 3, 7, 200, 13, 2, 50, 9]) + d[:4000])
    n += 1
print(n, "seeds in", out)
