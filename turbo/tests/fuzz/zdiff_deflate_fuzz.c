/* Copyright (C) 2026 Yingquan (Cody) Wu
 * For conditions of distribution and use, see copyright notice in zlib.h
 */

/* libFuzzer target: deflate of stock zlib (a shared library opened with dlopen, not instrumented) and of the patched
   zlib (linked in, with ASAN or UBSAN) must agree exactly: the same return code from every call and the same
   compressed bytes, for the same sequence of calls.
   Input: byte 0 level, 1 window bits choice, 2 memLevel, 3 strategy, 4 flags (bit 0 dictionary, bit 1 deflateParams
   mid-stream, bit 2 deflatePrime, bit 3 expand the payload into a long, repetitive input, bit 4 deflateReset and a
   second stream), 5 new level and strategy for deflateParams, then n + input piece sizes, n + flush modes, n +
   output piece sizes, then the payload. STOCK_SO names the library. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "zlib.h"

typedef struct {
    int (*deflateInit2_)(z_streamp, int, int, int, int, int, const char *, int);
    int (*deflate)(z_streamp, int);
    int (*deflateEnd)(z_streamp);
    int (*deflateParams)(z_streamp, int, int);
    int (*deflateSetDictionary)(z_streamp, const Bytef *, uInt);
    int (*deflatePrime)(z_streamp, int, int);
    int (*deflatePending)(z_streamp, unsigned *, int *);
    int (*deflateReset)(z_streamp);
    const char *(*zlibVersion)(void);
} Api;

static Api stock, fast = { deflateInit2_, deflate, deflateEnd, deflateParams, deflateSetDictionary, deflatePrime,
                           deflatePending, deflateReset, zlibVersion };

static void load_stock(void)
{
    void *h = dlopen(STOCK_SO, RTLD_NOW | RTLD_LOCAL);
    if (!h) { fprintf(stderr, "dlopen: %s\n", dlerror()); exit(1); }
#define GET(f) *(void **)&stock.f = dlsym(h, #f); if (!stock.f) { fprintf(stderr, "no %s\n", #f); exit(1); }
    GET(deflateInit2_) GET(deflate) GET(deflateEnd) GET(deflateParams) GET(deflateSetDictionary) GET(deflatePrime)
    GET(deflatePending) GET(deflateReset) GET(zlibVersion)
    fprintf(stderr, "stock %s, patched %s\n", stock.zlibVersion(), fast.zlibVersion());
}

typedef struct {                /* everything a run produces; both runs must produce the same */
    unsigned char *out;
    size_t n, cap;
    int codes[4096];
    size_t ncodes;
} Log;

static void put_code(Log *g, int r) { if (g->ncodes < 4096) g->codes[g->ncodes++] = r; }

static void put_out(Log *g, const unsigned char *p, size_t m)
{
    if (m == 0) return;
    if (g->n + m > g->cap) { g->cap = 2 * (g->n + m); g->out = realloc(g->out, g->cap); }
    memcpy(g->out + g->n, p, m);
    g->n += m;
}

typedef struct {
    int level, wbits, memlevel, strategy, flags, level2, strategy2, primebits, primeval;
    const unsigned char *isz, *fl, *osz, *in;
    size_t nisz, nfl, nosz, n, dictlen;
} Job;

/* deflate in until done with flush; output in pieces of the next output size */
static void pump(const Api *a, z_streamp s, int flush, const Job *j, size_t *ko, Log *g)
{
    unsigned char ob[2048];
    int guard = 0, r;
    do {
        size_t want = 1 + (j->nosz ? (size_t)j->osz[(*ko)++ % j->nosz] * 8 : 2047);
        if (j->n > 65536 && want < 256) want = 256;     /* keep long inputs from taking minutes */
        if (flush != Z_NO_FLUSH && flush != Z_FINISH && want < 8)
            want = 8;       /* zlib.h: avail_out must be greater than six when flushing, or the markers repeat */
        if (want > sizeof ob) want = sizeof ob;
        s->next_out = ob;
        s->avail_out = (unsigned)want;
        r = a->deflate(s, flush);
        put_code(g, r);
        put_out(g, ob, want - s->avail_out);
        if (r == Z_STREAM_END || r == Z_STREAM_ERROR) break;
        if (r == Z_BUF_ERROR && s->avail_out != 0) break;
    } while ((s->avail_out == 0 || s->avail_in != 0 || flush == Z_FINISH) && ++guard < 100000);
    {
        unsigned pend; int bits;
        put_code(g, a->deflatePending(s, &pend, &bits));
        put_code(g, (int)pend);
        put_code(g, bits);
    }
}

static void stream(const Api *a, z_streamp s, const Job *j, const unsigned char *in, size_t n, Log *g)
{
    size_t pos = 0, k = 0, kf = 0, ko = 0, piece = 0;
    static const int fmodes[6] = { Z_NO_FLUSH, Z_NO_FLUSH, Z_PARTIAL_FLUSH, Z_SYNC_FLUSH, Z_FULL_FLUSH, Z_BLOCK };
    while (pos < n) {
        size_t m = 1 + (j->nisz ? (size_t)j->isz[k++ % j->nisz] * 64 : 65535);
#ifndef KEEP_TINY_PIECES
        if (n > 65536 && m < 256) m = 256;      /* long inputs in tiny pieces only test the harness's patience */
#endif
        int flush = j->nfl ? fmodes[j->fl[kf++ % j->nfl] % 6] : Z_NO_FLUSH;
        if (m > n - pos) m = n - pos;
        if ((j->flags & 2) && piece == 1) {     /* may flush: give it room, keep what it writes */
            unsigned char pb[4096];
            s->next_in = (unsigned char *)in + pos;
            s->avail_in = 0;
            s->next_out = pb;
            s->avail_out = sizeof pb;
            put_code(g, a->deflateParams(s, j->level2, j->strategy2));
            put_out(g, pb, sizeof pb - s->avail_out);
        }
        s->next_in = (unsigned char *)in + pos;
        s->avail_in = (unsigned)m;
        pump(a, s, flush, j, &ko, g);
        pos += m - s->avail_in;
        if (s->avail_in == m) break;            /* no progress */
        piece++;
    }
    s->next_in = (unsigned char *)in + pos;
    s->avail_in = 0;
    pump(a, s, Z_FINISH, j, &ko, g);
}

static void run(const Api *a, const Job *j, Log *g)
{
    z_stream s;
    int r;
    memset(&s, 0, sizeof s);
    r = a->deflateInit2_(&s, j->level, Z_DEFLATED, j->wbits, j->memlevel, j->strategy, ZLIB_VERSION,
                         (int)sizeof(z_stream));
    put_code(g, r);
    if (r != Z_OK) return;
    if (j->flags & 1)
        put_code(g, a->deflateSetDictionary(&s, j->in, (uInt)j->dictlen));
    if (j->flags & 4)
        put_code(g, a->deflatePrime(&s, j->primebits, j->primeval));
    stream(a, &s, j, j->in, j->n, g);
    if (j->flags & 16) {                        /* a second stream on the same state */
        put_code(g, a->deflateReset(&s));
        stream(a, &s, j, j->in + j->n / 3, j->n - j->n / 3, g);
    }
    put_code(g, a->deflateEnd(&s));
}

#ifdef ZLIB_TURBO_BUILD
#include "zturbo.h"
static const Job *cur;

/* deflateSetDictionary() through a dictionary prepared on a scratch stream with the same parameters */
static int turbo_set_dictionary(z_streamp strm, const Bytef *dictionary, uInt len)
{
    z_stream t;
    z_dict *d = NULL;
    int r;
    memset(&t, 0, sizeof t);
    if (deflateInit2(&t, cur->level, Z_DEFLATED, cur->wbits, cur->memlevel, cur->strategy) != Z_OK)
        return deflateSetDictionary(strm, dictionary, len);
    r = deflatePrepareDictionary(&t, dictionary, len, &d);
    deflateEnd(&t);
    if (r != Z_OK)
        return deflateSetDictionary(strm, dictionary, len);
    r = deflateUseDictionary(strm, d);
    deflateFreeDictionary(d);
    return r;
}
#endif

#ifdef STANDALONE
#include <time.h>
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }
#endif

/* A long input made of copies of earlier parts and literals from the payload: long and short matches at all
   distances, and window slides. */
static size_t expand(const unsigned char *p, size_t n, unsigned char *out, size_t cap)
{
    unsigned long long x = 0x9E3779B97F4A7C15ULL;
    size_t i, o = 0;
    for (i = 0; i < n; i++) x = (x ^ p[i]) * 0x100000001B3ULL;
    for (i = 0; i < n && o < cap; i++) out[o++] = p[i];
    while (o < cap && o > 0) {
        size_t len, dist;
        x = x * 6364136223846793005ULL + 1442695040888963407ULL;
        if ((x >> 61) < 2) {                    /* a few literals */
            len = 1 + (size_t)((x >> 32) % 8);
            while (len-- && o < cap) out[o++] = n ? p[(x >> 16) % n] : (unsigned char)(x >> 40);
            continue;
        }
        len = 3 + (size_t)((x >> 20) % ((x >> 59) == 7 ? 300 : 40));
        dist = 1 + (size_t)((x >> 30) % ((x & 3) == 0 ? 40000 : (x & 3) == 1 ? 300 : 33000));
        if (dist > o) dist = o;
        while (len-- && o < cap) { out[o] = out[o - dist]; o++; }
    }
    return o;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    static const int wb[8] = { 15, -15, 31, 9, -9, 12, -13, 25 };
    static unsigned char *big;
    static Log g1, g2;
    Job j;
    size_t p = 6;

    if (!big) { load_stock(); big = malloc(400000); }
    if (size < 9) return 0;
    memset(&j, 0, sizeof j);
    j.level = data[0] % 11 - 1;                 /* -1 (default) to 9 */
    j.wbits = wb[data[1] & 7];
    j.memlevel = 1 + data[2] % 9;
    j.strategy = data[3] % 5;
    j.flags = data[4];
    j.level2 = data[5] % 10;
    j.strategy2 = (data[5] / 10) % 5;
    j.nisz = data[p++] % 16;
    if (p + j.nisz >= size) return 0;
    j.isz = data + p; p += j.nisz;
    j.nfl = data[p++] % 16;
    if (p + j.nfl >= size) return 0;
    j.fl = data + p; p += j.nfl;
    j.nosz = data[p++] % 16;
    if (p + j.nosz > size) return 0;
    j.osz = data + p; p += j.nosz;
    j.in = data + p;
    j.n = size - p;
    j.primebits = 1 + data[1] % 16;
    j.primeval = data[2] | (data[3] << 8);
    if (j.flags & 8) {
        j.n = expand(j.in, j.n, big, 1000 + (size_t)data[5] * 800);
        j.in = big;
    }
    j.dictlen = j.n < 300 ? j.n : (size_t)data[2] + 1;
    if (j.dictlen > j.n) j.dictlen = j.n;
    g1.n = g1.ncodes = 0;
    g2.n = g2.ncodes = 0;
#ifdef ZLIB_TURBO_BUILD
    cur = &j;                                   /* flag 32: the patched side uses a prepared dictionary */
    fast.deflateSetDictionary = (j.flags & 32) ? turbo_set_dictionary : deflateSetDictionary;
#endif
#ifdef STANDALONE
    {
        double t0 = now(), t1, t2;
        run(&stock, &j, &g1);
        t1 = now();
        run(&fast, &j, &g2);
        t2 = now();
        printf("level %d wbits %d mem %d strat %d flags %02x n %zu pieces %zu flushes %zu outs %zu: stock %.3f s,"
               " patched %.3f s, %zu bytes out\n", j.level, j.wbits, j.memlevel, j.strategy, j.flags, j.n, j.nisz,
               j.nfl, j.nosz, t1 - t0, t2 - t1, g1.n);
    }
#else
    run(&stock, &j, &g1);
    run(&fast, &j, &g2);
#endif
    if (g1.n != g2.n || g1.ncodes != g2.ncodes || memcmp(g1.out, g2.out, g1.n) ||
        memcmp(g1.codes, g2.codes, g1.ncodes * sizeof(int))) {
        size_t i = 0;
        while (i < g1.n && i < g2.n && g1.out[i] == g2.out[i]) i++;
        fprintf(stderr, "MISMATCH: level %d wbits %d mem %d strat %d flags %x n %zu: out %zu vs %zu (first diff %zu),"
                " codes %zu vs %zu\n", j.level, j.wbits, j.memlevel, j.strategy, j.flags, j.n, g1.n, g2.n, i,
                g1.ncodes, g2.ncodes);
        abort();
    }
    return 0;
}

#ifdef STANDALONE
/* the inputs given, each once, with the time of each side */
int main(int argc, char **argv)
{
    int i;
    for (i = 1; i < argc; i++) {
        FILE *f = fopen(argv[i], "rb");
        unsigned char *b;
        long n;
        if (!f) { perror(argv[i]); continue; }
        fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
        b = malloc(n + 1);
        if (fread(b, 1, n, f) != (size_t)n) return 1;
        fclose(f);
        printf("%s: ", argv[i]);
        fflush(stdout);
        LLVMFuzzerTestOneInput(b, (size_t)n);
        free(b);
    }
    return 0;
}
#endif
