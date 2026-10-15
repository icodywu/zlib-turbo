#!/bin/sh
# Prepared dictionaries: deflateUseDictionary() must give what deflateSetDictionary() gives.
#
# usage: turbo/tests/run_dict.sh STOCK_ZLIB_DIR TURBO_ZLIB_DIR FILE
#
# Builds turbo/tests/zdict.c against each tree and runs "zdict check FILE": pieces of FILE compressed with
# dictionaries taken from FILE, over levels, window sizes (zlib and raw), memLevels, strategies and dictionary
# lengths from 0 to beyond the window, a second message on each stream after deflateReset(). The hash of all
# return codes, total_in, Adler-32 values and compressed bytes must be the same for zlib's deflateSetDictionary(),
# zlib-turbo's deflateSetDictionary() and zlib-turbo's deflateUseDictionary(). FILE must be over 80 KB.
set -eu
[ $# -ge 3 ] || { sed -n '2,10p' "$0"; exit 1; }
HERE=$(cd "$(dirname "$0")" && pwd)
STOCK=$(cd "$1" && pwd); TURBO=$(cd "$2" && pwd); FILE=$3
CC=${CC:-cc}
WORK=${WORK:-./dict-work}
mkdir -p "$WORK"; WORK=$(cd "$WORK" && pwd)
SRCS="adler32.c compress.c crc32.c deflate.c infback.c inffast.c inflate.c inftrees.c trees.c uncompr.c zutil.c"
( cd "$STOCK" && $CC -O2 -I. $SRCS "$HERE/zdict.c" -o "$WORK/zdict_stock" )
( cd "$TURBO" && $CC -O2 -DZLIB_TURBO_BUILD -I. $SRCS "$HERE/zdict.c" -o "$WORK/zdict_turbo" )
s=$("$WORK/zdict_stock" check "$FILE"); t=$("$WORK/zdict_turbo" check "$FILE")
echo "zlib:       $s"; echo "zlib-turbo: $t"
a=$(echo "$s" | sed 's/.*path A \([0-9a-f]*\).*/\1/')
ta=$(echo "$t" | sed 's/.*path A \([0-9a-f]*\).*/\1/'); tb=$(echo "$t" | sed 's/.*path B \([0-9a-f]*\).*/\1/')
if [ "$a" = "$ta" ] && [ "$a" = "$tb" ]; then echo "all three agree"; else echo "DIFFERENT"; exit 1; fi
