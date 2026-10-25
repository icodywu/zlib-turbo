#!/bin/sh
# The same comparison as run_stress.sh on another architecture: static builds with a cross compiler, run under
# qemu-user (no sanitizers); zconsume.c on the 20 smallest files.
#
# usage: CC=aarch64-linux-gnu-gcc QEMU=qemu-aarch64 tests/run_cross.sh STOCK_ZLIB_DIR PATCHED_ZLIB_DIR INPUT_DIR...
#
# QEMU may be empty to run natively (e.g. CC="gcc -m32"). The dockcross images (https://github.com/dockcross)
# provide the cross compilers; we used linux-arm64, linux-armv5, linux-ppc64le and linux-s390x.
set -eu
[ $# -ge 3 ] || { sed -n '2,8p' "$0"; exit 1; }
HERE=$(cd "$(dirname "$0")" && pwd)
STOCK=$(cd "$1" && pwd); PATCHED=$(cd "$2" && pwd); shift 2
CC=${CC:-cc}; QEMU=${QEMU:-}
WORK=${WORK:-./cross-work}
mkdir -p "$WORK"; WORK=$(cd "$WORK" && pwd)
SRCS="adler32.c compress.c crc32.c deflate.c infback.c inffast.c inflate.c inftrees.c trees.c uncompr.c zutil.c"
FILES=$(for d in "$@"; do ls "$d"/*; done | xargs ls -S -r)

for b in stock patched; do
    dir=$STOCK; [ $b = patched ] && dir=$PATCHED
    ( cd "$dir" && $CC -O2 -static -I. $SRCS "$HERE/zstress.c" -o "$WORK/zstress_$b" )
    $QEMU "$WORK/zstress_$b" "$WORK/log_$b.txt" $FILES > "$WORK/out_$b.txt" 2>&1 || true
    echo "$b: $(tail -n 1 "$WORK/out_$b.txt")"
done
d=$(diff "$WORK/log_stock.txt" "$WORK/log_patched.txt" | grep -c '^>' || true)
echo "damaged streams with a different result: $d of $(wc -l < "$WORK/log_stock.txt")"
SMALL=$(for f in $FILES; do echo "$f"; done | head -n 20)
for b in stock patched; do
    dir=$STOCK; [ $b = patched ] && dir=$PATCHED
    ( cd "$dir" && $CC -O2 -static -I. $SRCS "$HERE/zconsume.c" -o "$WORK/zconsume_$b" )
    $QEMU "$WORK/zconsume_$b" $SMALL > "$WORK/consume_$b.txt" 2> /dev/null || true
done
c=$(diff "$WORK/consume_stock.txt" "$WORK/consume_patched.txt" | grep -c '^>' || true)
echo "decodes with different input or output left after a call: $c of $(wc -l < "$WORK/consume_stock.txt")"
[ "$d" = 0 ] && [ "$c" = 0 ] && echo "ALL OK" || { echo "FAILED"; exit 1; }
