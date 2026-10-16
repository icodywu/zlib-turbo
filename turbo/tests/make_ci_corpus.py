"""Two 4 MiB test files for CI, the same on every machine: "text", zlib's own sources and documents
concatenated (sorted by name, repeated to size), and "records", CSV-like rows of names, dates and numbers from a
fixed seed. Large enough for the threaded compressor's chunks and the cases with pieces of up to 3 MB.

usage: python3 turbo/tests/make_ci_corpus.py OUT_DIR   (run from the top of the source tree)
"""
import os
import random
import sys

SIZE = 4 << 20
out = sys.argv[1]
os.makedirs(out, exist_ok=True)

names = []
for top in (".", "doc", "examples", "test"):
    for f in sorted(os.listdir(top)):
        p = os.path.join(top, f)
        if os.path.isfile(p) and (f.endswith((".c", ".h", ".txt", ".html")) or f in ("ChangeLog", "FAQ", "README")):
            names.append(p)
src = b"".join(open(p, "rb").read().replace(b"\r\n", b"\n") for p in names)
text = (src * (SIZE // len(src) + 1))[:SIZE]
with open(os.path.join(out, "text"), "wb") as f:
    f.write(text)

random.seed(1)
words = sorted(set(src.split()))[:4000]
rows = []
n = 0
while n < SIZE:
    row = b"%d,%s,%04d-%02d-%02d,%d.%02d,%s\n" % (
        random.randrange(10 ** 6), random.choice(words)[:24], random.randrange(1990, 2027),
        random.randrange(1, 13), random.randrange(1, 29), random.randrange(10 ** 5), random.randrange(100),
        random.choice((b"open", b"closed", b"pending")))
    rows.append(row)
    n += len(row)
with open(os.path.join(out, "records"), "wb") as f:
    f.write(b"".join(rows)[:SIZE])
print("wrote text and records (%d bytes each) to %s" % (SIZE, out))
