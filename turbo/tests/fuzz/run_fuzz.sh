#!/bin/sh
# Differential fuzzing with libFuzzer: inflate of the stock zlib and of the patched zlib must agree on every
# input (return code, bytes out, the bytes), with random input and output piece sizes.
#
# usage: tests/fuzz/run_fuzz.sh STOCK_ZLIB_DIR PATCHED_ZLIB_DIR SEED_INPUT_DIR
#
# The stock zlib is a shared library opened with dlopen (built with -Bsymbolic so that its calls stay inside it,
# not instrumented); the patched zlib is linked into the fuzzer with SAN (default address; run again with
# SAN=undefined). Seeds: streams of the files in SEED_INPUT_DIR (tests/fuzz/make_seeds.py). TIME seconds
# (default 3600), JOBS workers (default: the number of CPUs). CLANG (default clang) must support -fsanitize=fuzzer.
set -eu
[ $# -ge 3 ] || { sed -n '2,11p' "$0"; exit 1; }
HERE=$(cd "$(dirname "$0")" && pwd)
STOCK=$(cd "$1" && pwd); PATCHED=$(cd "$2" && pwd); SEEDS=$(cd "$3" && pwd)
CLANG=${CLANG:-clang}; SAN=${SAN:-address}; TIME=${TIME:-3600}
JOBS=${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)}
WORK=${WORK:-./fuzz-work-$SAN}
mkdir -p "$WORK"; WORK=$(cd "$WORK" && pwd)
SRCS="adler32.c crc32.c deflate.c infback.c inffast.c inflate.c inftrees.c trees.c zutil.c"

( cd "$STOCK" && $CLANG -O2 -fPIC -shared -Wl,-Bsymbolic -I. $SRCS -o "$WORK/libzstock.so" )
FL="-g -O1 -fsanitize=fuzzer-no-link,$SAN"
[ "$SAN" = undefined ] && FL="$FL -fno-sanitize-recover=undefined"
mkdir -p "$WORK/obj"
( cd "$PATCHED" && for f in $SRCS; do $CLANG -c $FL -I. "$f" -o "$WORK/obj/${f%.c}.o"; done )
$CLANG -g -O1 -fsanitize=fuzzer,$SAN -I"$PATCHED" -DSTOCK_SO="\"$WORK/libzstock.so\"" \
    "$HERE/zdiff_fuzz.c" "$WORK"/obj/*.o -ldl -o "$WORK/zdiff"

mkdir -p "$WORK/corpus"
python3 "$HERE/make_seeds.py" "$SEEDS" "$WORK/corpus"
cd "$WORK"
./zdiff -jobs="$JOBS" -workers="$JOBS" -max_total_time="$TIME" -max_len=70000 -rss_limit_mb=4096 corpus > fuzz.log 2>&1 || true
runs=$(grep -h "^Done [0-9]* runs" fuzz-*.log 2>/dev/null | awk '{s += $2} END {print s + 0}')
bad=$(ls crash-* leak-* timeout-* 2>/dev/null | wc -l)
echo "$SAN: $runs runs in $JOBS workers, $bad findings$(grep -l MISMATCH fuzz-*.log 2>/dev/null | head -n 1 | sed 's/^/, first mismatch in /')"
[ "$bad" = 0 ]
