#!/bin/sh
# inflate() writes nothing past the output space it is given, at every split of the output (turbo/tests/zbound.c),
# with the fast loop and with zlib's loop (Z_INFLATE_FAST64=0).
#
# usage: turbo/tests/run_bound.sh TURBO_ZLIB_DIR [SEEDS]
#
# Runs zbound with seeds 1 to SEEDS (default 3). SAN=address,undefined builds with sanitizers.
set -eu
[ $# -ge 1 ] || { sed -n '2,7p' "$0"; exit 1; }
HERE=$(cd "$(dirname "$0")" && pwd)
TURBO=$(cd "$1" && pwd)
SEEDS=${2:-3}
CC=${CC:-cc}
WORK=${WORK:-./bound-work}
mkdir -p "$WORK"; WORK=$(cd "$WORK" && pwd)
SRCS="adler32.c compress.c crc32.c deflate.c infback.c inffast.c inflate.c inftrees.c trees.c uncompr.c zutil.c"
FLAGS="-O2"
[ -n "${SAN:-}" ] && FLAGS="-O1 -g -fsanitize=$SAN -fno-sanitize-recover=all"
( cd "$TURBO" && $CC $FLAGS -I. $SRCS "$HERE/zbound.c" -o "$WORK/zbound" )
( cd "$TURBO" && $CC $FLAGS -DZ_INFLATE_FAST64=0 -I. $SRCS "$HERE/zbound.c" -o "$WORK/zbound0" )
fail=0
s=1
while [ "$s" -le "$SEEDS" ]; do
    "$WORK/zbound" "$s" || fail=1
    "$WORK/zbound0" "$s" || fail=1
    s=$((s + 1))
done
exit $fail
