#!/bin/sh
# Multithreaded compression: deflateSetThreads() must not change one output byte or return code.
#
# usage: turbo/tests/run_threads.sh TURBO_ZLIB_DIR THREADS FILE...
#
# Builds turbo/tests/zmt.c against the tree, with threads, and runs "zmt check THREADS FILE...": each file (a
# prefix of it for the slow cases) at levels 4 to 9 with many settings, input and output piece sizes, flushes,
# deflateParams() changes, dictionaries and input moved between calls, once with one thread and once with
# THREADS, comparing every output byte and return code. One thread is zlib's output (run_compress.sh checks
# that), so this compares the threads with zlib. Then, without a sanitizer, "zmt count THREADS FILE" checks on
# Linux that THREADS threads run at levels 4 to 9 (from 3) and one at the others. SAN=thread, SAN=address,undefined builds
# with a sanitizer. "zmt speed LEVEL THREADS FILE..." measures the speed.
set -eu
[ $# -ge 3 ] || { sed -n '2,12p' "$0"; exit 1; }
HERE=$(cd "$(dirname "$0")" && pwd)
TURBO=$(cd "$1" && pwd); THREADS=$2; shift 2
CC=${CC:-cc}
WORK=${WORK:-./threads-work}
mkdir -p "$WORK"; WORK=$(cd "$WORK" && pwd)
SRCS="adler32.c compress.c crc32.c deflate.c infback.c inffast.c inflate.c inftrees.c trees.c uncompr.c zutil.c"
FLAGS="-O2"
[ -n "${SAN:-}" ] && FLAGS="-O1 -g -fsanitize=$SAN -fno-sanitize-recover=all"
( cd "$TURBO" && $CC $FLAGS -DZ_TURBO_THREADS -pthread -I. $SRCS "$HERE/zmt.c" -o "$WORK/zmt" )
"$WORK/zmt" check "$THREADS" "$@"
[ -n "${SAN:-}" ] || "$WORK/zmt" count "$THREADS" "$1"
