#!/bin/sh
# The deflate changes of zlib 1.4 where zlib-turbo has code of its own: Z_STREAM_END for a stream that fills the
# output space exactly, deflateResetKeep() in the middle of a stream, and deflateCopy() at levels 10 to 12 before a
# change of level.
#
# usage: turbo/tests/run_edge.sh STOCK_ZLIB_DIR TURBO_ZLIB_DIR FILE...
#
# Builds turbo/tests/zedge.c against stock zlib and, with threads, against zlib-turbo, and runs both on each file.
# The "exact" lines of levels 0 to 9 must be zlib's. Every "reset" stream of zlib-turbo must inflate to its input,
# and four threads must give the bytes of one; where zlib-turbo's bytes after deflateResetKeep() differ from
# zlib's, the count is shown with the number of zlib's streams that inflate to the wrong data. SAN=address,undefined
# builds zlib-turbo with sanitizers.
set -eu
[ $# -ge 3 ] || { sed -n '2,12p' "$0"; exit 1; }
HERE=$(cd "$(dirname "$0")" && pwd)
STOCK=$(cd "$1" && pwd); TURBO=$(cd "$2" && pwd); shift 2
CC=${CC:-cc}
WORK=${WORK:-./edge-work}
mkdir -p "$WORK"; WORK=$(cd "$WORK" && pwd)
SRCS="adler32.c compress.c crc32.c deflate.c infback.c inffast.c inflate.c inftrees.c trees.c uncompr.c zutil.c"
FLAGS="-O2"
[ -n "${SAN:-}" ] && FLAGS="-O1 -g -fsanitize=$SAN -fno-sanitize-recover=all"
( cd "$STOCK" && $CC -O2 -I. $SRCS "$HERE/zedge.c" -o "$WORK/zedge_stock" )
( cd "$TURBO" && $CC $FLAGS -DTURBO -DZ_TURBO_THREADS -pthread -I. $SRCS "$HERE/zedge.c" -o "$WORK/zedge_turbo" )
fail=0
for f in "$@"; do
    "$WORK/zedge_stock" "$f" > "$WORK/stock.txt"
    if ! "$WORK/zedge_turbo" "$f" > "$WORK/turbo.txt"; then
        grep FAIL "$WORK/turbo.txt"; fail=1
    fi
    low='level [0-9] '
    grep "^exact $low" "$WORK/turbo.txt" > "$WORK/t_exact.txt" || true
    grep "^exact $low" "$WORK/stock.txt" > "$WORK/s_exact.txt" || true
    grep '^reset .*threads 1 ' "$WORK/turbo.txt" > "$WORK/t_one.txt" || true
    grep '^reset .*threads 4 ' "$WORK/turbo.txt" | sed 's/threads 4 /threads 1 /' > "$WORK/t_four.txt" || true
    grep "^reset $low.*threads 1 " "$WORK/turbo.txt" | sed 's/ round trip.*//' > "$WORK/t_reset.txt" || true
    grep "^reset $low.*threads 1 " "$WORK/stock.txt" | sed 's/ round trip.*//' > "$WORK/s_reset.txt" || true
    r="$f:"
    if cmp -s "$WORK/t_exact.txt" "$WORK/s_exact.txt"; then
        r="$r exact: zlib's ($(wc -l < "$WORK/s_exact.txt") cases);"
    else
        r="$r exact: DIFFERENT from zlib;"; fail=1
    fi
    if cmp -s "$WORK/t_one.txt" "$WORK/t_four.txt"; then
        r="$r reset: 4 threads = 1 ($(wc -l < "$WORK/t_one.txt") cases),"
    else
        r="$r reset: 4 threads DIFFERENT from 1,"; fail=1
    fi
    r="$r $(grep -c '^reset.*round trip ok' "$WORK/turbo.txt" || true) of $(grep -c '^reset.*round trip' "$WORK/turbo.txt" || true) round trips ok,"
    r="$r $(diff "$WORK/t_reset.txt" "$WORK/s_reset.txt" | grep -c '^<' || true) differ from zlib,"
    r="$r zlib's wrong in $(grep -c '^reset.*threads 1 .*round trip FAIL' "$WORK/stock.txt" || true);"
    r="$r copy: $(grep -c '^copy.*round trip ok' "$WORK/turbo.txt" || true) ok"
    echo "$r"
done
exit $fail
