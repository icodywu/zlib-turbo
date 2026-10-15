#!/bin/sh
# Levels 10 to 12: every stream must decompress to its input, also with stock zlib.
#
# usage: turbo/tests/run_opt.sh STOCK_ZLIB_DIR TURBO_ZLIB_DIR FILE...
#
# Builds turbo/tests/zopt.c against zlib-turbo and runs "zopt check FILE...": each file at levels 10 to 12 with 21
# settings (input and output in pieces down to one byte, sync and full flushes, window sizes, memLevels, raw, zlib
# and gzip wrappers, strategies, dictionaries, deflateCopy, deflateParams among the levels), each decompressed and
# compared, and the streams fed in pieces compared with the stream made in one call. Then each file's level 12
# stream is decompressed by stock zlib's minigzip. SAN=address,undefined builds with sanitizers.
# "zopt ratio LEVEL FILE..." gives sizes and speed.
set -eu
[ $# -ge 3 ] || { sed -n '2,11p' "$0"; exit 1; }
HERE=$(cd "$(dirname "$0")" && pwd)
STOCK=$(cd "$1" && pwd); TURBO=$(cd "$2" && pwd); shift 2
CC=${CC:-cc}
WORK=${WORK:-./opt-work}
mkdir -p "$WORK"; WORK=$(cd "$WORK" && pwd)
SRCS="adler32.c compress.c crc32.c deflate.c infback.c inffast.c inflate.c inftrees.c trees.c uncompr.c zutil.c"
GZ="gzclose.c gzlib.c gzread.c gzwrite.c"
FLAGS="-O2"
[ -n "${SAN:-}" ] && FLAGS="-O1 -g -fsanitize=$SAN -fno-sanitize-recover=all"
( cd "$TURBO" && $CC $FLAGS -I. $SRCS "$HERE/zopt.c" -o "$WORK/zopt" )
( cd "$STOCK" && $CC -O2 -DHAVE_UNISTD_H -I. $SRCS $GZ test/minigzip.c -o "$WORK/minigzip_stock" )
"$WORK/zopt" check "$@"
for f in "$@"; do
    "$WORK/zopt" write 12 "$f" "$WORK/t.z" > /dev/null
    # minigzip reads gzip: wrap the raw deflate data of the zlib stream in a gzip header and trailer
    python3 - "$WORK/t.z" "$WORK/t.gz" "$f" <<'EOF'
import struct, sys, zlib
z = open(sys.argv[1], 'rb').read()
data = open(sys.argv[3], 'rb').read()
open(sys.argv[2], 'wb').write(b'\x1f\x8b\x08\x00\x00\x00\x00\x00\x00\xff' + z[2:-4] +
                              struct.pack('<II', zlib.crc32(data) & 0xffffffff, len(data) & 0xffffffff))
EOF
    "$WORK/minigzip_stock" -d -c "$WORK/t.gz" | cmp - "$f" && echo "$f: stock zlib decompresses level 12"
done
