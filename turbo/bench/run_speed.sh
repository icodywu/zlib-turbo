#!/bin/sh
# Decompression speed of two zlib source trees on the same streams.
#
# usage: bench/run_speed.sh STOCK_ZLIB_DIR PATCHED_ZLIB_DIR CORPUS_DIR [CORPUS_DIR ...] > results.txt
#
# For each level in LEVELS (default "1 6 9") the stock zlib compresses every file of every corpus whole
# (file -> file.z, in a work directory), then zbench decodes them with each build: in one inflate() call
# (mode 0) and in 16 KiB input / 64 KiB output pieces (mode 1). ROUNDS (default 2) rounds are interleaved;
# bench/summarize.py takes the best round of each configuration.
#
# Environment: CC (default cc), CFLAGS (default -O2), WORK (default ./speed-work), ROUNDS, LEVELS,
# CORE (default 2: the benchmark runs under "taskset -c $CORE" when taskset exists; CORE= to run unpinned).
set -eu
[ $# -ge 3 ] || { sed -n '2,15p' "$0"; exit 1; }
HERE=$(cd "$(dirname "$0")" && pwd)
STOCK=$(cd "$1" && pwd); PATCHED=$(cd "$2" && pwd); shift 2
CC=${CC:-cc}; CFLAGS=${CFLAGS:--O2}; WORK=${WORK:-./speed-work}; ROUNDS=${ROUNDS:-2}; LEVELS=${LEVELS:-"1 6 9"}
CORE=${CORE-2}
mkdir -p "$WORK"; WORK=$(cd "$WORK" && pwd)
SRCS="adler32.c compress.c crc32.c deflate.c infback.c inffast.c inflate.c inftrees.c trees.c uncompr.c zutil.c"

for b in stock patched; do
    dir=$STOCK; [ $b = patched ] && dir=$PATCHED
    ( cd "$dir" && $CC $CFLAGS -I. $SRCS "$HERE/zbench.c" -o "$WORK/zbench_$b" )
done
( cd "$STOCK" && $CC $CFLAGS -I. $SRCS "$HERE/zmake.c" -o "$WORK/zmake" )

PIN=""
[ -n "$CORE" ] && command -v taskset > /dev/null && PIN="taskset -c $CORE"
echo "# $(uname -sm); $($CC --version | head -n 1); CFLAGS=$CFLAGS; pinned: ${PIN:-no}"

for lev in $LEVELS; do
    for c in "$@"; do
        name=$(basename "$c")
        d="$WORK/L$lev/$name"
        mkdir -p "$d"
        for f in "$c"/*; do [ -f "$f" ] && cp "$f" "$d/"; done
        "$WORK/zmake" "$lev" $(ls "$d"/* | grep -v '\.z$') > /dev/null
    done
done

r=1
while [ $r -le "$ROUNDS" ]; do
    for lev in $LEVELS; do
        for c in "$@"; do
            name=$(basename "$c")
            files=$(ls "$WORK/L$lev/$name"/* | grep -v '\.z$')
            for b in stock patched; do
                for m in 0 1; do
                    mbs=$($PIN "$WORK/zbench_$b" $m $files | tail -n 1 | sed 's/.* \([0-9.]*\) MB\/s.*/\1/')
                    echo "round=$r level=$lev corpus=$name build=$b mode=$m MB/s=$mbs"
                done
            done
        done
    done
    r=$((r + 1))
done
