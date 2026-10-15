"""Best round of each configuration from run_speed.sh output (or results/*.txt), as a Markdown table of
stock -> patched and the gain, one table per compiler setting.

usage: python3 bench/summarize.py results.txt
"""
import sys
from collections import defaultdict

best = defaultdict(float)
order, settings = [], []
for line in open(sys.argv[1]):
    if line.startswith("#") or "MB/s=" not in line:
        continue
    f = dict(kv.split("=", 1) for kv in line.split())
    cflags = f.get("cflags", "")
    mode = {"0": "one-call", "1": "16K/64K"}.get(f["mode"], f["mode"])
    key = (f["corpus"], f["level"])
    if key not in order:
        order.append(key)
    if cflags not in settings:
        settings.append(cflags)
    k = (cflags,) + key + (f["build"], mode)
    best[k] = max(best[k], float(f["MB/s"]))

for cflags in settings:
    if cflags:
        print("\n%s\n" % cflags)
    print("| corpus | level | one call (MB/s) | 16K in / 64K out (MB/s) |")
    print("|---|---|---|---|")
    for corpus, level in order:
        cells = []
        for mode in ("one-call", "16K/64K"):
            a, b = best[(cflags, corpus, level, "stock", mode)], best[(cflags, corpus, level, "patched", mode)]
            cells.append("%.0f -> %.0f (%+.0f%%)" % (a, b, (b / a - 1) * 100) if a and b else "-")
        if cells != ["-", "-"]:
            print("| %s | %s | %s | %s |" % (corpus, level, cells[0], cells[1]))
