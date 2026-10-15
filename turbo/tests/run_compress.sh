#!/bin/sh
# Identical compressed output and compression speed: zlib against zlib-turbo.
#
# usage: turbo/tests/run_compress.sh STOCK_ZLIB_DIR TURBO_ZLIB_DIR INPUT_DIR [INPUT_DIR ...]
#
# Builds turbo/bench/zcbench.c against each tree (CC, default cc; CFLAGS, default -O2), compresses every file of the
# input directories at levels 0 to 9, and compares a hash of all compressed bytes: they must be the same at every
# level. Prints the speed of each (best of at least two runs, at least one second per file).
set -eu
[ $# -ge 3 ] || { sed -n '2,8p' "$0"; exit 1; }
HERE=$(cd "$(dirname "$0")" && pwd)
STOCK=$(cd "$1" && pwd); TURBO=$(cd "$2" && pwd); shift 2
CC=${CC:-cc}; CFLAGS=${CFLAGS:--O2}
WORK=${WORK:-./compress-work}
mkdir -p "$WORK"; WORK=$(cd "$WORK" && pwd)
SRCS="adler32.c compress.c crc32.c deflate.c infback.c inffast.c inflate.c inftrees.c trees.c uncompr.c zutil.c"
FILES=$(for d in "$@"; do ls "$d"/*; done)
for b in stock turbo; do
    dir=$STOCK; [ $b = turbo ] && dir=$TURBO
    ( cd "$dir" && $CC $CFLAGS -I. $SRCS "$HERE/../bench/zcbench.c" -o "$WORK/zcbench_$b" )
done
fail=0
for level in 0 1 2 3 4 5 6 7 8 9; do
    a=$("$WORK/zcbench_stock" $level $FILES)
    b=$("$WORK/zcbench_turbo" $level $FILES)
    ha=${a##*hash }; hb=${b##*hash }
    sa=$(echo "$a" | sed 's/.*comp \([0-9.]*\) MB.*/\1/'); sb=$(echo "$b" | sed 's/.*comp \([0-9.]*\) MB.*/\1/')
    if [ "$ha" = "$hb" ]; then same=same; else same=DIFFERENT; fail=1; fi
    echo "level $level: output $same; zlib $sa MB/s, zlib-turbo $sb MB/s"
done
exit $fail
