#!/bin/sh
# Correctness of the patched zlib against the stock one, under AddressSanitizer and UndefinedBehaviorSanitizer.
#
# usage: tests/run_stress.sh STOCK_ZLIB_DIR PATCHED_ZLIB_DIR INPUT_DIR [INPUT_DIR ...]
#
# Builds tests/zstress.c against each tree (CC, default cc; SANFLAGS, default ASAN+UBSAN; DEFS, extra -D options
# such as -DINFLATE_STRICT), runs it on every file of the input directories (smallest first), and compares the
# two logs of damaged streams: return code, output length and CRC-32 of the output must be the same for every
# damaged stream. Then tests/zconsume.c (-O2, no sanitizer): the return code, input and output left after every
# inflate() call, and the input used at each stream end, must be the same. Also builds and runs tests/syncflush.c
# with the patched tree.
set -eu
[ $# -ge 3 ] || { sed -n '2,11p' "$0"; exit 1; }
HERE=$(cd "$(dirname "$0")" && pwd)
STOCK=$(cd "$1" && pwd); PATCHED=$(cd "$2" && pwd); shift 2
CC=${CC:-cc}
SANFLAGS=${SANFLAGS:-"-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer"}
DEFS=${DEFS:-}
WORK=${WORK:-./stress-work}
mkdir -p "$WORK"; WORK=$(cd "$WORK" && pwd)
SRCS="adler32.c compress.c crc32.c deflate.c infback.c inffast.c inflate.c inftrees.c trees.c uncompr.c zutil.c"
FILES=$(for d in "$@"; do ls "$d"/*; done | xargs ls -S -r)

for b in stock patched; do
    dir=$STOCK; [ $b = patched ] && dir=$PATCHED
    ( cd "$dir" && $CC $SANFLAGS $DEFS -I. $SRCS "$HERE/zstress.c" -o "$WORK/zstress_$b" )
done
fail=0
for b in stock patched; do
    "$WORK/zstress_$b" "$WORK/log_$b.txt" $FILES > "$WORK/out_$b.txt" 2> "$WORK/err_$b.txt" || fail=1
    echo "$b: $(tail -n 1 "$WORK/out_$b.txt")"
    n=$(grep -c "runtime error\|ERROR: AddressSanitizer" "$WORK/err_$b.txt" || true)
    [ "$n" = 0 ] || { echo "$b: $n sanitizer reports, see $WORK/err_$b.txt"; fail=1; }
done
d=$(diff "$WORK/log_stock.txt" "$WORK/log_patched.txt" | grep -c '^>' || true)
echo "damaged streams with a different result: $d of $(wc -l < "$WORK/log_stock.txt")"
[ "$d" = 0 ] || fail=1

for b in stock patched; do
    dir=$STOCK; [ $b = patched ] && dir=$PATCHED
    ( cd "$dir" && $CC -O2 $DEFS -I. $SRCS "$HERE/zconsume.c" -o "$WORK/zconsume_$b" )
    "$WORK/zconsume_$b" $FILES > "$WORK/consume_$b.txt" 2> /dev/null || fail=1
done
d=$(diff "$WORK/consume_stock.txt" "$WORK/consume_patched.txt" | grep -c '^>' || true)
echo "decodes with different input or output left after a call: $d of $(wc -l < "$WORK/consume_stock.txt")"
[ "$d" = 0 ] || fail=1

( cd "$PATCHED" && $CC -O2 -I. $SRCS "$HERE/syncflush.c" -o "$WORK/syncflush" )
"$WORK/syncflush" $FILES | tail -n 1
[ $fail = 0 ] && echo "ALL OK" || { echo "FAILED"; exit 1; }
