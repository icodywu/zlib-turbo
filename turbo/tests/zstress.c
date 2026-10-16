/* Copyright (C) 2026 Yingquan (Cody) Wu
 * For conditions of distribution and use, see copyright notice in zlib.h
 */

/* zstress: inflate correctness on the linked zlib (stock or patched), deterministic, for ASAN/UBSAN builds.
   usage: zstress log file...
   For each file and each deflate configuration below (deflate is the same code in both builds, so the streams are the
   same), it decodes the stream (1) in one call, (2) with random input and output chunk sizes, each chunk in a buffer
   of its own allocated to its exact size (so a read before next_in or past the chunk, or a write past avail_out, is
   caught), (3) with inflateBack and the same kind of chunks, and checks the output. Then it decodes damaged copies
   (bit flips, bytes set, truncations) with random chunks and writes, per copy, the return code, total_out and the
   CRC-32 of the output to the log; the logs of two builds must be identical. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "zlib.h"

static unsigned long long rng = 88172645463325252ull;
static void seed(unsigned a, unsigned b, unsigned c) { rng = 88172645463325252ull ^ ((unsigned long long)a << 40) ^ ((unsigned long long)b << 20) ^ c; for (int i = 0; i < 4; i++) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; } }
static unsigned rnd(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return (unsigned)(rng >> 11); }
static unsigned chunk(void) { unsigned k = rnd() % 17; return 1 + rnd() % (1u << k); }   /* 1 .. 64 Ki, log-uniform */

static int fails = 0;
#define FAIL(...) do { printf("FAIL: " __VA_ARGS__); printf("\n"); fails++; } while (0)

typedef struct { const char* name; int level, wbits, strategy; } Config;
static const Config cfg[] = {
    { "L1",      1, 15, Z_DEFAULT_STRATEGY }, { "L6",   6, 15, Z_DEFAULT_STRATEGY }, { "L9", 9, 15, Z_DEFAULT_STRATEGY },
    { "L6w9",    6,  9, Z_DEFAULT_STRATEGY }, { "L6w12", 6, 12, Z_DEFAULT_STRATEGY },
    { "raw",     6, -15, Z_DEFAULT_STRATEGY }, { "gzip", 6, 31, Z_DEFAULT_STRATEGY },
    { "huff",    6, 15, Z_HUFFMAN_ONLY }, { "rle", 6, 15, Z_RLE }, { "fixed", 6, 15, Z_FIXED }, { "stored", 0, 15, Z_DEFAULT_STRATEGY },
};
#define NCFG (int)(sizeof cfg / sizeof cfg[0])

static unsigned char* deflate_buf(const unsigned char* p, size_t n, const Config* c, size_t* cn)
{
    z_stream s; memset(&s, 0, sizeof s);
    if (deflateInit2(&s, c->level, Z_DEFLATED, c->wbits, 8, c->strategy) != Z_OK) { FAIL("deflateInit2"); exit(1); }
    size_t cap = deflateBound(&s, n) + 64;
    unsigned char* out = malloc(cap);
    s.next_in = (unsigned char*)p; s.avail_in = (uInt)n; s.next_out = out; s.avail_out = (uInt)cap;
    if (deflate(&s, Z_FINISH) != Z_STREAM_END) { FAIL("deflate"); exit(1); }
    *cn = s.total_out;
    deflateEnd(&s);
    return out;
}

/* one call */
static int inflate_once(const unsigned char* c, size_t cn, int wbits, unsigned char* o, size_t on, size_t* got)
{
    z_stream s; memset(&s, 0, sizeof s);
    if (inflateInit2(&s, wbits) != Z_OK) return -99;
    s.next_in = (unsigned char*)c; s.avail_in = (uInt)cn; s.next_out = o; s.avail_out = (uInt)on;
    int r = inflate(&s, Z_FINISH);
    *got = s.total_out;
    inflateEnd(&s);
    return r;
}

/* random chunks, each in its own exact-size buffer; output appended to o (capacity on) */
static int inflate_chunks(const unsigned char* c, size_t cn, int wbits, unsigned char* o, size_t on, size_t* got)
{
    z_stream s; memset(&s, 0, sizeof s);
    if (inflateInit2(&s, wbits) != Z_OK) return -99;
    size_t pos = 0, total = 0;
    unsigned char* inbuf = NULL;
    int r = Z_OK, stall = 0;
    for (;;) {
        if (s.avail_in == 0 && pos < cn) {
            size_t k = chunk(); if (k > cn - pos) k = cn - pos;
            free(inbuf); inbuf = malloc(k); memcpy(inbuf, c + pos, k); pos += k;
            s.next_in = inbuf; s.avail_in = (uInt)k;
        }
        size_t k = chunk();
        unsigned char* ob = malloc(k);
        s.next_out = ob; s.avail_out = (uInt)k;
        const uInt before_in = s.avail_in;
        r = inflate(&s, Z_NO_FLUSH);
        size_t made = k - s.avail_out;
        if (total + made > on) { free(ob); r = -98; break; }
        memcpy(o + total, ob, made); total += made;
        free(ob);
        if (r != Z_OK && r != Z_BUF_ERROR) break;
        if (made == 0 && s.avail_in == before_in && (pos >= cn || s.avail_in)) { if (++stall > 3) break; } else stall = 0;
    }
    free(inbuf);
    *got = total;
    inflateEnd(&s);
    return r;
}

/* inflateBack with random chunks */
typedef struct { const unsigned char* c; size_t cn, pos; unsigned char* buf; unsigned char* o; size_t on, total; int over; } Back;
static unsigned in_fn(void* d, z_const unsigned char** next)
{
    Back* b = d;
    size_t k = chunk(); if (k > b->cn - b->pos) k = b->cn - b->pos;
    free(b->buf); b->buf = malloc(k ? k : 1); memcpy(b->buf, b->c + b->pos, k); b->pos += k;
    *next = b->buf;
    return (unsigned)k;
}
static int out_fn(void* d, unsigned char* p, unsigned len)
{
    Back* b = d;
    if (b->total + len > b->on) { b->over = 1; return 1; }
    memcpy(b->o + b->total, p, len); b->total += len;
    return 0;
}
static int inflate_back(const unsigned char* c, size_t cn, unsigned char* o, size_t on, size_t* got)
{
    z_stream s; memset(&s, 0, sizeof s);
    unsigned char* window = malloc(1 << 15);
    if (inflateBackInit(&s, 15, window) != Z_OK) return -99;
    Back b = { c, cn, 0, NULL, o, on, 0, 0 };
    int r = inflateBack(&s, in_fn, &b, out_fn, &b);
    inflateBackEnd(&s);
    free(b.buf); free(window);
    *got = b.total;
    return r;
}

static unsigned char* load(const char* name, size_t* n)
{
    FILE* f = fopen(name, "rb");
    if (!f) { perror(name); exit(1); }
    fseek(f, 0, SEEK_END); *n = (size_t)ftell(f); fseek(f, 0, SEEK_SET);
    unsigned char* p = malloc(*n + 1);
    if (fread(p, 1, *n, f) != *n) { perror(name); exit(1); }
    fclose(f);
    return p;
}

int main(int argc, char** argv)
{
    FILE* log = fopen(argv[1], "w");
    long checks = 0, damaged = 0;
    for (int i = 2; i < argc; i++) {
        size_t n;
        unsigned char* p = load(argv[i], &n);
        const char* base = strrchr(argv[i], '/') ? strrchr(argv[i], '/') + 1 : argv[i];
        unsigned char* o = malloc(n + 65536);
        for (int k = 0; k < NCFG; k++) {
            const Config* cf = &cfg[k];
            size_t cn, got;
            unsigned char* c = deflate_buf(p, n, cf, &cn);
            const int iw = cf->wbits;
            int r = inflate_once(c, cn, iw, o, n + 65536, &got);
            if (r != Z_STREAM_END || got != n || memcmp(o, p, n)) FAIL("%s %s one call: r=%d got=%zu", base, cf->name, r, got);
            checks++;
            for (int rep = 0; rep < 3; rep++) {
                seed((unsigned)i, (unsigned)k, 1 + (unsigned)rep);
                r = inflate_chunks(c, cn, iw, o, n + 65536, &got);
                if (r != Z_STREAM_END || got != n || memcmp(o, p, n)) FAIL("%s %s chunks %d: r=%d got=%zu", base, cf->name, rep, r, got);
                checks++;
            }
            seed((unsigned)i, (unsigned)k, 9);
            if (cf->wbits > 0 && cf->wbits <= 15 && cn > 6) {           /* zlib wrapper: the raw deflate data follows 2 bytes */
                r = inflate_back(c + 2, cn - 2, o, n + 65536, &got);
                if (r != Z_STREAM_END || got != n || memcmp(o, p, n)) FAIL("%s %s inflateBack: r=%d got=%zu", base, cf->name, r, got);
                checks++;
            }
            /* damaged copies of (at most) the first 64 KiB of the stream */
            size_t dn = cn < 65536 ? cn : 65536;
            unsigned char* d = malloc(dn);
            for (int m = 0; m < (dn > 3 && !getenv("NODAMAGE") ? 60 : 0); m++) {
                seed((unsigned)i, (unsigned)k, 100 + (unsigned)m);
                memcpy(d, c, dn);
                size_t len = dn;
                const unsigned kind = rnd() % 3;
                if (kind == 0) for (int b = 0, nb = 1 + rnd() % 3; b < nb; b++) { size_t at = 2 + rnd() % (dn - 2); d[at] ^= (unsigned char)(1u << (rnd() % 8)); }
                else if (kind == 1) { size_t at = 2 + rnd() % (dn - 2); d[at] = (unsigned char)rnd(); }
                else len = 2 + rnd() % (dn - 2);
                r = inflate_chunks(d, len, iw, o, n + 65536, &got);
                fprintf(log, "%s %s %d %d %d %zu %08lx\n", base, cf->name, m, kind, r, got, crc32(0, o, (uInt)got));
                damaged++;
            }
            free(d); free(c);
        }
        printf("%s: %zu bytes done\n", base, n);
        fflush(stdout);
        free(p); free(o);
    }
    fclose(log);
    printf("%s: %ld checks, %ld damaged streams, %d failures\n", zlibVersion(), checks, damaged, fails);
    return fails != 0;
}
