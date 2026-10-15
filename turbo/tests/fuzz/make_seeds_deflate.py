"""Seed corpus for zdiff_deflate_fuzz.c from a directory of files: pieces of the files, each with random parameters
(level, window bits, memLevel, strategy, dictionary, deflateParams, deflatePrime, expansion into a long input,
deflateReset, prepared dictionary) and random lists of input piece sizes, flush modes and output piece sizes.

usage: python3 turbo/tests/fuzz/make_seeds_deflate.py INPUT_DIR CORPUS_DIR [COUNT]
"""
import os
import random
import sys

src, out = sys.argv[1], sys.argv[2]
count = int(sys.argv[3]) if len(sys.argv) > 3 else 3000
os.makedirs(out, exist_ok=True)
random.seed(5)
data = []
for f in sorted(os.listdir(src)):
    path = os.path.join(src, f)
    if os.path.isfile(path):
        data.append(open(path, "rb").read()[:2000000])


def lst():
    m = random.randrange(16)
    return bytes([m]) + bytes(random.randrange(256) for _ in range(m))


for k in range(count):
    d = random.choice(data)
    a = random.randrange(max(1, len(d) - 60000))
    payload = d[a:a + random.choice((100, 2000, 20000, 60000))]
    hdr = bytes([random.randrange(11), random.randrange(8), random.randrange(9), random.randrange(5),
                 random.randrange(64), random.randrange(50)])
    with open(os.path.join(out, "s%05d" % k), "wb") as f:
        f.write(hdr + lst() + lst() + lst() + payload)
print(count, "seeds")
