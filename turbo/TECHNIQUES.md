# How zlib-turbo is faster

zlib-turbo follows one rule: change *how* zlib does its work, never *what* it decides. Everything that shapes the
compressed stream stays as it is in zlib 1.3.2:

- the hash function and the hash chains;
- the chain limits and the lazy-match rules of each level;
- where blocks end;
- how Huffman trees are built.

The first six changes are in how bits are read and written, how bytes are compared and copied, and how a checksum is
summed. The seventh lets worker threads find zlib's matches ahead of its parse and write its blocks, while the parse
itself stays zlib's. The eighth adds levels 10 to 12, the one place with output of its own, and the ninth, optional,
computes the checksums with SIMD instructions. Apart from the threads, none of the techniques is new: libdeflate,
Zstandard, WZIP and others use them, and the sections below name the ones borrowed. The work was fitting them into
zlib so that not one output byte, return code or error message changes, in any build configuration, and checking
that by comparing with unmodified zlib (see [How it was tested](../README.md#how-it-was-tested)).

All measurements here, except those with threads in section 7, are on one core of an AMD EPYC 9334 (Zen 4, 2.7 GHz),
GCC 14.2 `-O2`, best of two runs. They cover Silesia, Canterbury + Calgary and enwik8, each file compressed whole.
Speeds are MB/s of uncompressed data.

## What each change gives

Each row adds one commit to the row above it.

| Silesia, MB/s | Compression, level 1 | level 6 | level 9 | Decompression, level 6 |
|---|---:|---:|---:|---:|
| zlib 1.3.2 | 114 | 31.9 | 13.1 | 438 |
| + 1. decoder | 114 | 31.9 | 13.0 | 698 (**+60%**) |
| + 2. Adler-32 | 115 | 32.0 | 13.1 | 739 (**+6%**) |
| + 3. match comparisons | 123 (**+7%**) | 34.3 (**+7%**) | 13.9 (**+7%**) |  |
| + 4. bit writer | 134 (**+9%**) | 35.0 (**+2%**) | 14.0 |  |
| + 5. hash-table slide | 151 (**+13%**) | 36.0 (**+3%**) | 14.2 | 738 |

The table is for Silesia; Canterbury + Calgary and enwik8 show the same pattern. Changes 3 to 5 leave
decompression as it is (the last row, within 0.3%). All runs, for the three corpora:
[`results/steps_epyc9334.txt`](results/steps_epyc9334.txt).

## 1. Decompression: a 64-bit bit buffer, larger tables and wide copies

*`inffast.c`, `inftrees.h`, `inflate.c`, `infback.c`*

**What zlib does.** `inflate_fast()` decodes while at least 6 bytes of input and 258 bytes of output space remain.
Its bit buffer is refilled two bytes at a time, one byte per step, whenever fewer than 15 bits are left. Refills
come before each literal/length code, before each distance code and before extra bits. Codes are looked up in a
first-level table of 9 bits (literals and lengths) or 6 bits (distances). Longer codes need a second lookup. A
match is copied three bytes per loop step.

**What zlib-turbo does.** A new loop runs first, while at least 16 bytes of input and 273 bytes of output space
remain. It hands over to zlib's original loop, unchanged, for the ends of the buffers.

- **Branch-free refill.** One unaligned eight-byte load fills the buffer:

  ```c
  hold8 |= load64(in) << bits;
  in += (63 - bits) >> 3;
  bits |= 56;
  ```

  After a refill there are at least 56 bits. That covers a whole length/distance pair: at most 15 + 5 + 15 + 13 =
  48 bits, so it decodes with no further checks. It also covers up to three literals found in the first-level
  table. This is variant 4 of Fabian Giesen's [*Reading bits in far too many
  ways*](https://fgiesen.wordpress.com/2018/02/20/reading-bits-in-far-too-many-ways-part-2/), and libdeflate uses
  the same approach.
- **Larger first-level tables.** They are 10 bits for literals and lengths and 8 bits for distances, so second
  lookups are rare.
  - The table space was recomputed with zlib's own `examples/enough.c`: 1332 + 400 entries instead of 852 + 592.
    With `PKZIP_BUG_WORKAROUND` it is 1334 + 402.
  - The cost is 1,152 more bytes per inflate stream, and filling 1,280 first-level entries per dynamic block
    instead of 576.
- **Wide match copies.**
  - Matches at distance 16 or more are copied 16 bytes at a time, and at distance 8 to 15 eight at a time.
  - A distance of 1 (a run of one byte) spreads the byte across a 64-bit word and stores it eight bytes at a time.
  - Distances 2 to 7 copy eight bytes and advance by the distance. The first *distance* bytes of each step are
    right, and the next step overwrites the rest.
  - A copy may write up to 15 bytes past the end of the match. That is why the loop needs 258 + 15 bytes of space,
    and those bytes are overwritten by what follows.

**Why the results are the same.** The tables decode the same codes; only the split between first and second level
differs. Every check runs at the same point with the same message:

- invalid literal/length code;
- invalid distance code;
- distance too far back;
- `INFLATE_STRICT`'s limit;
- the `INFLATE_ALLOW_INVALID_DISTANCE_TOOFAR_ARRR` zero fill.

When the loop ends, it gives back the whole bytes it read ahead, leaving fewer than eight bits in the buffer as
zlib's loop does. It never reads past the input or writes past the output buffer. Without a 64-bit integer type,
the new loop is not compiled.

This change is also offered to zlib as [pull request #793](https://github.com/madler/zlib/pull/793).

## 2. Adler-32, eight bytes per step

*`adler32.c`*

**What zlib does.** For each byte it updates `a += byte` and `sum += a`. Each of these additions depends on the one
before, which limits how much the processor can overlap, even with the loop unrolled 16 times. It reduces modulo
65521 every 5,552 bytes. That runs at 3.9 GB/s here.

Adler-32 is computed over all uncompressed data of the zlib format, in both directions. Once the decoder above is
faster, Adler-32 at 3.9 GB/s takes about a fifth of decompression time.

**What zlib-turbo does.** It works on eight bytes at once, inside one 64-bit integer (SWAR: SIMD within a
register). Eight bytes b₀…b₇ add b₀ + … + b₇ to `a`, and 8·a + 8b₀ + 7b₁ + … + 1b₇ to `sum`.

1. Masking with `0x00ff00ff00ff00ff` puts the even bytes and the odd bytes in four 16-bit lanes each.
2. One 64-bit multiplication by a constant forms a dot product in its top 16 bits: (v₀, v₁, v₂, v₃) × (c₀, c₁, c₂,
   c₃) gives v₀c₃ + v₁c₂ + v₂c₁ + v₃c₀. No partial sum can carry into the next lane, because none reaches 2¹⁶ (at
   most 255 × 20).
3. Two such products give the weighted sum of the even bytes and of the odd bytes, and a third gives the plain sum.

That is three multiplications per eight bytes. The 64-bit accumulators reduce modulo 65521 only every 2²⁰ bytes.

**Speed and where it is used.** The result is the same number, computed in a different order: 5.55 GB/s against
3.88 with GCC 14 (+43%), and 4.44 GB/s with clang 18 (+14%).

It is on by default only where 64-bit multiplication is fast: x86-64 and AArch64, including 64-bit MSVC. On 32-bit
x86, where it would be emulated, it measured 1.55 GB/s against zlib's 3.42, so it stays off there.
`-DZ_ADLER_SWAR=0` or `=1` overrides the choice.

The gzip format uses CRC-32, which zlib already computes several bytes at a time, and raw deflate has no checksum.
Those formats gain from change 1 only.

## 3. Match finding: two- and eight-byte comparisons

*`deflate.c`, `longest_match()`*

**What zlib does.** For each candidate on the hash chain, zlib first compares four single bytes:

- bytes `best_len` and `best_len - 1`, because a candidate that differs there cannot beat the current best match;
- bytes 0 and 1.

It then compares one byte at a time, unrolled eight times, up to 258 bytes.

**What zlib-turbo does.**

- The four bytes are compared as two 16-bit values. The bytes compared and the candidates rejected are the same,
  with half the loads and branches.
- The rest is compared eight bytes at a time. Two words are XORed, and the first differing byte is found with
  count-trailing-zeros (`__builtin_ctzll` on GCC and clang, `_BitScanForward64` on 64-bit MSVC). Big-endian GCC
  targets count leading zeros instead, and other compilers fall back to a byte loop.
- This is how Zstandard's `ZSTD_count()` works.

**Why the output is the same.** The chain is walked the same way: the same order, the same `max_chain` limit, the
same reduction past `good_match` and the same early exit at `nice_match`. Each candidate is accepted or rejected as
before, and its length is the same, up to the same limit of 258. As in zlib, the length is then capped to the bytes
actually available. Like zlib's own comparison, it reads no further than 258 bytes from the current position,
which zlib keeps inside the window.

## 4. A 64-bit bit writer

*`trees.c`, `compress_block()`*

**What zlib does.** It keeps a 16-bit bit buffer in the stream state. Every code and every group of extra bits goes
through `send_bits()`, which checks whether the buffer is full and, when it is, writes two bytes. A match costs up
to four such calls.

**What zlib-turbo does.** The loop that writes a block's literals and matches keeps a 64-bit buffer in a local
variable.

- One symbol, either a literal or a whole match (at most 48 bits), is added with no checks.
- One eight-byte store then writes all the complete bytes, and the 0 to 7 remaining bits stay in the buffer.
- The store can reach up to seven bytes past the output so far. Those bytes lie in `pending_buf`, which zlib
  shares with the symbols still to be read. zlib's own analysis of that overlay, in `deflateInit2_()`, keeps the
  output at least 139 bits (17 bytes) behind the next unread symbol.

The block headers and the code-length trees still use zlib's writer, since they are a small part of the output.
WZIP and Zstandard write their bits the same way.

**Why the output is the same.** The same bits are written in the same order; only the buffering differs. A
`ZLIB_DEBUG` build checks the bit count of every block, and passes.

## 5. Sliding the hash tables

*`deflate.c`, `slide_hash()`*

**What zlib does.** Each time the window moves by 32 KiB, every entry of the hash table and of the chain table
(32,768 entries each at the default settings) drops by 32,768. Entries that would go negative become 0. zlib's loop
runs backwards through a pointer, one entry at a time, and compilers keep it scalar. At level 1 it took about 18%
of the time, in a profile taken after changes 2 to 4.

**What zlib-turbo does.** It writes the same update as eight independent entries per step, walking forwards.
Compilers then process eight entries per instruction: GCC 12 or later at `-O2`, and clang, on x86 and ARM. On x86,
`m >= w ? m - w : 0` on 16-bit values becomes a single saturating subtraction, `psubusw`. The values are the same.
GCC 11 does not vectorize loops at `-O2`, so with it the slide stays scalar.

## 6. Prepared dictionaries

*`deflate.c`, new header `zturbo.h`*

**What zlib does.** `deflateSetDictionary()` copies the dictionary into the window and inserts every position of it
into the hash chains. For a 32 KiB dictionary that is about 32,000 insertions, each a dependent write to a random
place in a 64 KiB table. It takes about 33 µs, and it repeats for every stream. Compressing a 256-byte message
takes far less than that, so with small messages the indexing is most of the work.

**What zlib-turbo adds.** `deflatePrepareDictionary()` indexes the dictionary once, on a stream just initialized.
It then keeps a snapshot of the result:

- the hash table;
- the chain entries of the dictionary's positions;
- the rolling hash value and the count of positions not yet inserted;
- the dictionary itself and its Adler-32.

`deflateUseDictionary()` copies that snapshot into a stream, in about 5 µs, and repeats the rest of what
`deflateSetDictionary()` does: the zeroing past the data in the window, `total_in`, the block start and the match
state. The state then equals what `deflateSetDictionary()` would have left, so the output is the same.

**When it applies.** The snapshot depends only on `windowBits` and `memLevel`. One prepared dictionary therefore
serves every level and strategy. It is read-only once made, so any number of streams in any number of threads can
use it at once.

On a stream it does not fit, `deflateUseDictionary()` calls `deflateSetDictionary()` itself, with the same result
and the same return value:

- a stream with other `windowBits` or `memLevel`;
- a stream that already has data (zlib adds the dictionary to a raw deflate stream, and refuses it in the zlib
  format);
- a gzip stream, where zlib refuses a dictionary.

**Why only compression.** `inflateSetDictionary()` only copies the dictionary into the window and builds no index,
so decompression needs nothing new.

## 7. Multithreaded compression

*`deflate.c`, `trees.c`, `zturbo.h`; `deflateSetThreads()`*

**Why the matches can be found ahead.** At levels 4 to 9 (`deflate_slow()`), zlib inserts every position into
the hash chains, whatever matches it chooses. The chain at a position therefore holds every earlier position with
the same hash: it is a fact of the input, not of the parse. `longest_match()` then depends on the window and on
only two things the parse decides. One is `prev_length`, the match to beat, which only filters out matches that
are not longer. The other is whether `prev_length` reached `good_match`, which cuts the chain to a quarter. So the
longest match at a position, over the whole chain or over its first quarter, can be found by anyone who has the
input. Levels 1 to 3 insert only short matches' positions, so their chains depend on the parse, and they stay on
one thread.

**What the workers do.** The input is cut into chunks of 256 KiB. Each worker copies a chunk, with the 32 KiB
window before it, builds its own hash chains for it, and runs `deflate_slow()`'s parse over it, starting 4 KiB
early. The parse resynchronizes with the real one quickly. From then on the worker searches exactly the positions
the real parse will ask for, with the chain length it will use, and records each result. The chains are kept as
16-bit distances to the previous position with the same hash, which halves their memory.

**What the calling thread does.** It runs zlib's parse unchanged. At each position it takes the worker's result if
it was found with the chain length it needs (99.99% of the time), and searches itself otherwise. That happens
before a worker resynchronizes and where a search could see the end of the available input. While the chunks
cover the parse, it skips zlib's own hash insertions and slides. It catches up, inserting the last 32 KiB of
positions, before it needs the chains itself, and before `deflate()` returns. The state is then zlib's again.

**Blocks written on a worker.** When a block's symbol buffer is full, the calling thread hands the symbols and
their counts to a worker, which builds the trees and writes the block, and goes on parsing into a second buffer.
The catch is zlib's API: `deflate()` must return where zlib returns, with the same pending output and bits. zlib
returns after a block when the output space runs out. So a block goes to a worker only if the output space left
surely takes it: the cost of the block in the static trees, plus a margin, bounds every way zlib can write it. A
block that might not fit is written by the calling thread, as zlib does. The worker writes into zlib's
`pending_buf`, which the calling thread does not touch until it takes the block back, at the next block or before
`deflate()` returns. In case a stored block is smallest, the block's input is copied, since the window can slide
meanwhile.

**Edge cases.** Full flushes and dictionaries that replace the window start the chunks again with no history.
After levels 0 to 3, `Z_HUFFMAN_ONLY` or `Z_RLE`, whose chains are incomplete, the workers start only 32 KiB past
the switch back. When `deflate()` returns, the workers have copied all the input they read, so the application may
reuse its buffer. The one new rule is that input left over must be passed again unchanged.

**What it gives.** Silesia / enwik8, MB/s, each file whole, threads in all (the calling thread and the workers)
on one 32-core socket, the median of three nodes:

| | 1 thread | 3 | 4 | 8 | 16 | 24 |
|---|---:|---:|---:|---:|---:|---:|
| Level 6 | 35.8 / 27.1 | 64 / 51 | 92 / 76 | 166 / 172 | 183 / 207 | 161 / 193 |
| Level 9 | 14.2 / 20.7 | 26 / 40 | 38 / 59 | 75 / 135 | 115 / 209 | 114 / 201 |

A single worker gains nothing: the calling thread then searches the chunks the worker has not reached itself, on
top of its parse, and was 3% to 15% slower than with no worker at levels 6 and 9. So two threads run as one, and
the workers start only at levels 4 to 9, at the first `deflate()` call there. Writing blocks on workers added 17%
to 23% with 16 workers: it had taken a quarter of the calling thread's time. The calling thread is now the limit at
about 210 MB/s, spending its time choosing matches in zlib's order, which no
thread can do ahead. At level 9 on Silesia the workers are the limit, as long chains there take many steps per
position.

## 8. Levels 10 to 12

*`deflate.c` (`deflate_opt()` and the `opt_` functions), `zturbo.h`*

These levels are the one place where zlib-turbo decides what to output, so they do not follow the rule above: no
zlib level gives their output to match. They are chosen only by asking for a level above 9, and they share nothing
with levels 0 to 9 but the stream wrappers, the flushes and the bit writer's state.

**Segments.** The input is kept until 256 KiB have come (or a flush), then parsed as a segment, with the 32 KiB
before it as the window. Segments end only every 256 KiB and at flushes, so the output does not depend on how the
input is fed. The levels write into an output buffer of their own, made `pending_buf` while they run, which holds
a whole segment's blocks; zlib's flushes, wrappers, `deflatePending()` and `deflateCopy()` then work unchanged.

**Matches.** A binary tree of the window's strings, ordered from four-byte hashes as in libdeflate, gives at each
position the longest match within a search depth (24, 64 and 256 nodes at levels 10, 11 and 12) and on the way the
nearest match of each shorter length. A table of three-byte hashes adds the nearest three-byte match. All of them
are kept for the segment, a few per position. The tree and the tables persist from segment to segment: they hold
positions plus a base that grows as the window moves, so that entries older than the window read as empty without
clearing anything, and a position enters the tree only once 258 bytes follow it.

**Parse.** For given bit costs of every literal, length and distance code, a pass from the end of the segment to
the start finds the cheapest way to every position, trying each match at every length up to its own. The first
costs are the entropies of a lazy parse's symbols (zlib's: a match is dropped when the next position has a longer
one, and three-byte matches farther than 4 KiB are not taken); the parse then runs once more with the entropies of
its own result.

**Blocks.** The symbols are split into blocks by dynamic programming over up to 256 candidate boundaries, each
block costed as the entropy of its symbol counts plus an estimate of its header. Each block is then parsed again
with the bit lengths of its own Huffman codes, which are exact costs, until the block no longer gets smaller (up to
2, 4 and 8 times); level 12 then splits and reparses once more. Adjacent blocks merge when one block is smaller. Each
block is written with the smallest of a dynamic, a fixed and a stored block, and the whole segment is stored when
that is smaller still, which keeps `deflateBound()` true. Code lengths come from package-merge, optimal within the
15-bit (and, for the code length code, 7-bit) limits; the code lengths are run-length coded by dynamic programming
with the code length code's own costs, three rounds.

**What it gives.** On Silesia, enwik8 and Canterbury + Calgary, level 12 is 0.15% to 0.33% smaller than libdeflate
1.26 at its level 12, and 4.0% to 5.7% smaller than zlib's level 9, at about 3 MB/s, half of libdeflate's speed. It
is 0.2% smaller than Zopfli on Silesia and 0.1% to 0.2% larger on the other two, 6 to 11 times as fast. Level 10, at
5.5 MB/s, is as small as libdeflate's level 12 on Silesia. Table in the [README](../README.md#levels-10-to-12).

## 9. SIMD checksums, optional

*`crc32.c`, `adler32.c`; `configure --simd`, CMake `ZLIB_TURBO_SIMD`, or `-DZ_TURBO_SIMD` with the target flags*

**Why the checksums.** With the faster decoder, checking the data costs more of decompression than before: zlib's
CRC-32 runs at 2.77 GB/s, about a quarter of the time of gzip decompression, and Adler-32 at 5.5 GB/s (change 2)
an eighth of zlib-format decompression. Both are fixed functions of the bytes, so faster code changes nothing else.

**CRC-32.** Carry-less multiplication (PCLMULQDQ) folds the data into a 128-bit remainder, as in Intel's "Fast CRC
Computation for Generic Polynomials Using PCLMULQDQ Instruction": four 16-byte lanes are each folded 64 bytes ahead
with the constants x^544 and x^480 modulo the polynomial, then into one lane, then 16 bytes at a time, and a
Barrett reduction gives the 32-bit CRC. Inputs of 64 bytes or more take this path; the last 0 to 15 bytes and short
inputs take zlib's braided code. 15.3 GB/s instead of 2.77.

**Adler-32.** AVX2 adds 32 bytes per step: `vpsadbw` sums the bytes into `a`, and `vpmaddubsw` with the weights
32 to 1 into `sum`, with `a` from the steps before added 32 times by a running sum of the lanes. The lanes are
added up and reduced modulo 65521 every 5536 bytes, the largest stretch whose sums fit 32 bits when every byte is
255, including the 32-bit `unsigned long` of 64-bit Windows. 41 GB/s instead of 5.5.

**Why opt-in.** Without run-time CPU detection, which zlib-turbo does not do, a library built with these
instructions fails on processors without them. Distributions build for a baseline that lacks AVX2; people who build
for their own machines, or for x86-64-v3, can turn them on. The default build stays plain C.

**What it gives.** Decompression, level 6 streams in one call: gzip +20% to +28%, zlib +11% to +16% over zlib-turbo
without SIMD, and +91% to +114% over zlib 1.3.2. Compression gains 1% to 5%, most at level 1 with gzip. Table in
the [README](../README.md#simd-checksums-optional).

## What was tried and dropped

Each was kept only if it was faster (for levels 10 to 12: smaller) without being worse elsewhere.

| Idea | Where it comes from | Result |
|---|---|---|
| Prefetch the next candidate of the hash chain | Zstandard | -1% to -3% at levels 1 and 6, down to -10% at level 9 |
| Check the two byte pairs of a candidate in one branch | Zstandard's branch-free candidate checks | up to +2% at level 1, -1% at level 6, -1% to -5% at level 9 |
| Check the eight bytes that end at the best length, not two | Zstandard's hash-chain search | -5% to -7% at level 1, -2% at level 3, within 1% at levels 6 and 9 |
| Hash each position from its three bytes in the insertion loops, instead of rolling the hash | Zstandard | within 0.7% |
| Write a whole length/distance pair into the bit buffer at once | Zstandard's combined codes | -2% to +0.6% |
| Copy short-distance matches as Zstandard does (`ZSTD_overlapCopy8`) | Zstandard | decompression -0.2% to -1.3% |
| Copy 32 bytes per step after the first 16 | Zstandard's copy loop | within 0.7% |
| Larger first-level decoding tables: 9 to 11 bits for distances, 11 for literals and lengths | | -0.1% to -1.3% on average (11-bit lengths: within 1%); second lookups were already rare, 0.3-0.4% of distances and 1-2% of other codes |
| Fetch the next code before copying a match | common in fast decoders | 32-bit x86 -5% to -8%; 64-bit with GCC 14 -1% to -3% in one call, +1% to +2% in pieces; clang +4% |
| Index the dictionary faster in `deflateSetDictionary()` | | at most 1% to 3%: indexing takes about 2.5 cycles per position, bound by memory writes |
| Clear only the hash slots a small stream used, in `deflateReset()` | | clearing takes 0.6 µs of the 4 to 9 µs a message of 64 to 256 bytes costs; not worth the code |
| Threads: search every position for every possible match to beat, instead of following the parse | | 1.8 MB/s at level 9 with two threads, far below one thread: most of the work is for positions the parse skips |
| Threads: 32-bit chain links instead of 16-bit distances | | the same speed at 8 and 16 threads, 3% to 8% slower at 4, twice the memory |
| Levels 10 to 12: first costs from a greedy parse (longest match everywhere) | | 1.5% larger on mr and 0.8% on reymont: the parse keeps the greedy parse's habits |
| Levels 10 to 12: a 15-bit three-byte hash table | libdeflate | 0.7% larger on x-ray: two strings in one slot lose three-byte matches; 20 bits |
| Levels 10 to 12: entropy costs in the block passes, and no stop when a pass does not help | Zopfli | 0.01% smaller, 1.2 to 1.9 times the time |
| Levels 10 to 12: deeper search, more passes, finer block boundaries, 1 MiB segments | | each within 0.05% on mr, x-ray and reymont |
| Levels 10 to 12: the tree and the hash tables rebuilt for each segment | | the same sizes; a flush every few bytes cost a 4 MB clear and 32K insertions each |

Compression and decompression speed on Silesia, Canterbury + Calgary and enwik8 as above, each against the version
kept.

## What is not changed, and why

**Decisions that shape the output.** Most faster zlib-compatible libraries change these (Chromium's zlib keeps zlib's
hash, and its output, when built with `USE_ZLIB_RABIN_KARP_HASH`; README, "Speed"):

- the hash function;
- how the match search works and how far it looks;
- lazy matching, block splitting and the level parameters.

That is how zlib-ng compresses two to three times as fast, with output that differs from zlib's. zlib-turbo keeps
every one of these decisions, so its output stays byte-identical at levels 0 to 9. The only different output is
that of levels 10 to 12, which zlib does not have; a faster but different compressor could come later only as such
an option, never as the default.

**SIMD code, assembly and run-time CPU detection.** The default build has none of them; the SIMD checksums
(change 9) come only when the build asks for them, and nothing detects the CPU at run time. More of it would make
the code larger, harder to review and harder to port, which is the opposite of what a drop-in zlib needs. The only
compiler-specific code is the count-trailing-zeros call in change 3, and it has a portable fallback. Compilers
targeting newer CPUs generate better code from the same source anyway: built with `-march=x86-64-v3` (AVX2, BMI2),
zlib-turbo decompresses another 2.5% to 5.7% faster, and compresses within 1% as fast.
