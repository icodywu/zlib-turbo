"""Synthetic inputs for zstress.c, next to real corpora: runs, short periods (copies at distances 2..13), random
bytes, text interleaved with runs and random bursts, and tiny files. Deterministic (seed 1).

usage: python3 tests/make_inputs.py OUT_DIR [TEXT_FILE]
"""
import os
import random
import sys

out = sys.argv[1]
os.makedirs(out, exist_ok=True)
random.seed(1)


def put(name, data):
    with open(os.path.join(out, name), "wb") as f:
        f.write(data)


put("zeros", bytes(1 << 20))
put("random", bytes(random.getrandbits(8) for _ in range(1 << 20)))
for p in (2, 3, 4, 5, 6, 7, 9, 13):
    unit = bytes(random.getrandbits(8) for _ in range(p))
    put("period%d" % p, (unit * ((1 << 19) // p + 1))[: 1 << 19])
txt = open(sys.argv[2], "rb").read() if len(sys.argv) > 2 else bytes(random.choice(b"etaoin shrdlu\n") for _ in range(1 << 16))
mix = bytearray()
while len(mix) < (1 << 20):
    k = random.randrange(4)
    if k == 0:
        a = random.randrange(max(1, len(txt) - 4096))
        mix += txt[a:a + random.randrange(1, 4096)]
    elif k == 1:
        mix += bytes([random.getrandbits(8)]) * random.randrange(1, 600)
    elif k == 2:
        mix += bytes(random.getrandbits(8) for _ in range(random.randrange(1, 300)))
    elif mix:
        mix += mix[-random.randrange(1, min(len(mix), 32768) + 1):][: random.randrange(3, 300)]
put("mixed", bytes(mix[: 1 << 20]))
for n in (0, 1, 2, 7, 8, 9, 257, 258, 259):
    put("tiny%d" % n, bytes(random.getrandbits(8) for _ in range(n)))
print("wrote", len(os.listdir(out)), "files to", out)
