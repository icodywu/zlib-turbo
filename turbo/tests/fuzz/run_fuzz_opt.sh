#!/bin/sh
# Fuzzing of levels 10 to 12 with libFuzzer: every stream zlib-turbo makes must decompress with stock zlib to its input,
# and one made in one call must fit in deflateBound() (window sizes, memLevels, strategies, flush modes, deflateParams,
# dictionaries, deflateCopy, deflateReset, long inputs, input and output in pieces).
#
# usage: turbo/tests/fuzz/run_fuzz_opt.sh STOCK_ZLIB_DIR TURBO_ZLIB_DIR SEED_INPUT_DIR
#
# The stock zlib is a shared library opened with dlopen (built with -Bsymbolic, not instrumented); zlib-turbo is
# linked into the fuzzer with SAN (default address; run again with SAN=undefined). TIME seconds (default 3600),
# JOBS workers (default: the number of CPUs). CLANG (default clang) must support -fsanitize=fuzzer.
set -eu
[ $# -ge 3 ] || { sed -n '2,10p' "$0"; exit 1; }
HERE=$(cd "$(dirname "$0")" && pwd)
STOCK=$(cd "$1" && pwd); TURBO=$(cd "$2" && pwd); SEEDS=$(cd "$3" && pwd)
CLANG=${CLANG:-clang}; SAN=${SAN:-address}; TIME=${TIME:-3600}
JOBS=${JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)}
WORK=${WORK:-./fuzz-opt-work-$SAN}
mkdir -p "$WORK"; WORK=$(cd "$WORK" && pwd)
SRCS="adler32.c crc32.c deflate.c infback.c inffast.c inflate.c inftrees.c trees.c zutil.c"

( cd "$STOCK" && $CLANG -O2 -fPIC -shared -Wl,-Bsymbolic -I. $SRCS -o "$WORK/libzstock.so" )
FL="-g -O1 -fsanitize=fuzzer-no-link,$SAN"
[ "$SAN" = undefined ] && FL="$FL -fno-sanitize-recover=undefined"
mkdir -p "$WORK/obj"
( cd "$TURBO" && for f in $SRCS; do $CLANG -c $FL -I. "$f" -o "$WORK/obj/${f%.c}.o"; done )
$CLANG -g -O1 -fsanitize=fuzzer,$SAN -I"$TURBO" -DZLIB_TURBO_BUILD -DSTOCK_SO="\"$WORK/libzstock.so\"" \
    "$HERE/zopt_fuzz.c" "$WORK"/obj/*.o -ldl -o "$WORK/zopt_fuzz"

mkdir -p "$WORK/corpus"
python3 "$HERE/make_seeds_deflate.py" "$SEEDS" "$WORK/corpus"
cd "$WORK"
./zopt_fuzz -jobs="$JOBS" -workers="$JOBS" -max_total_time="$TIME" -max_len=70000 -rss_limit_mb=4096 \
    -timeout=300 corpus > fuzz.log 2>&1 || true
runs=$(grep -h "^Done [0-9]* runs" fuzz-*.log 2>/dev/null | awk '{s += $2} END {print s + 0}')
bad=$(ls crash-* leak-* 2>/dev/null | wc -l)
slow=$(ls timeout-* oom-* 2>/dev/null | wc -l)
echo "$SAN: $runs runs in $JOBS workers, $bad findings, $slow slow inputs$(grep -l MISMATCH fuzz-*.log 2>/dev/null | head -n 1 | sed 's/^/, first mismatch in /')"
[ "$bad" = 0 ]
