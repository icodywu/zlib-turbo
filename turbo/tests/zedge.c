/* Copyright (C) 2026 Yingquan (Cody) Wu
 * For conditions of distribution and use, see copyright notice in zlib.h
 */

/* zedge: the deflate changes of zlib 1.4 where zlib-turbo has code of its own.
   zedge FILE    one line per case. Built against stock zlib, and against
                 zlib-turbo with -DTURBO (levels 10 to 12, and four threads).

   exact   deflate(Z_FINISH) into exactly the space the stream needs, in one
           call: zlib.h promises Z_STREAM_END, also for raw deflate. The lines
           of levels 0 to 9 must be the same as stock zlib's.
   reset   deflateResetKeep() in the middle of a stream, with input still in
           the lookahead, which it drops. The deflate data after it, with the
           window before it as dictionary, must inflate to the input given
           after it ("round trip ok"). zlib 1.4 can fail this: its hash
           chains keep the last two positions under hashes of the dropped
           bytes, and longest_match() then takes a third byte as equal that
           is not. zlib-turbo takes those positions out of the chains, so its
           bytes can differ from zlib 1.4's here; four threads must give the
           bytes of one.
   copy    (zlib-turbo) deflateCopy() at levels 10 to 12, then the copy to
           levels 0, 1, 6 and 9 before compressing, with little output space
           per call; the stream must decompress to its input. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "deflate.h"            /* the lookahead dropped by deflateResetKeep() */
#ifdef TURBO
#  include "zturbo.h"
#endif
#if defined(TURBO) && !defined(FASTEST)
#  define MORE                  /* threads, levels 10 to 12 */
#  define TOP 12
#else
#  define TOP 9
#endif

static int fails;

static unsigned char *load(const char *name, size_t *len) {
    FILE *f = fopen(name, "rb");
    unsigned char *b;
    long n;
    if (f == NULL) { perror(name); exit(1); }
    fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
    b = malloc(n + 1);
    if (fread(b, 1, n, f) != (size_t)n) { perror(name); exit(1); }
    fclose(f);
    *len = n;
    return b;
}

static unsigned long long fnv(unsigned long long h, const unsigned char *p,
                              size_t n) {
    while (n--) h = (h ^ *p++) * 1099511628211ULL;
    return h;
}

static void init(z_stream *s, int level, int wbits, int threads) {
    memset(s, 0, sizeof(*s));
    if (deflateInit2(s, level, Z_DEFLATED, wbits, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
        printf("init fail level %d\n", level); exit(1);
    }
#ifdef MORE
    if (threads > 1 && deflateSetThreads(s, threads) != Z_OK) {
        printf("threads fail\n"); exit(1);
    }
#else
    (void)threads;
#endif
}

/* Inflate raw deflate data in[0..n), with dict[0..dn) as dictionary if dn,
   and compare with want[0..wn). */
static int inflates_to(const unsigned char *in, size_t n,
                       const unsigned char *dict, size_t dn,
                       const unsigned char *want, size_t wn) {
    z_stream s;
    unsigned char *out = malloc(wn + 1);
    int ret, ok;
    memset(&s, 0, sizeof(s));
    if (inflateInit2(&s, -15) != Z_OK) { printf("inflateInit fail\n"); exit(1); }
    if (dn && inflateSetDictionary(&s, dict, (uInt)dn) != Z_OK) {
        printf("inflateSetDictionary fail\n"); exit(1);
    }
    s.next_in = (unsigned char *)in; s.avail_in = (uInt)n;
    s.next_out = out; s.avail_out = (uInt)wn + 1;
    ret = inflate(&s, Z_FINISH);
    ok = ret == Z_STREAM_END && s.total_out == wn && memcmp(out, want, wn) == 0;
    inflateEnd(&s);
    free(out);
    return ok;
}

/* The deflate data of a zlib or gzip stream in p[0..n): its offset, and its
   length in *len. */
static size_t payload(const unsigned char *p, size_t n, int wbits, size_t *len) {
    size_t head = wbits < 0 ? 0 : wbits > 15 ? 10 : (p[1] & 0x20) ? 6 : 2;
    size_t tail = wbits < 0 ? 0 : wbits > 15 ? 8 : 4;
    *len = n - head - tail;
    return head;
}

/* Feed in[0..len) in pieces of size bytes; after the piece that passes a
   third of it, deflateResetKeep(). Output in pieces of 4096 bytes. */
static void reset_case(const unsigned char *in, size_t len, int level, int wbits,
                       int threads, size_t size) {
    z_stream s;
    size_t cap = len + len / 8 + 4096, got = 0, mark = 0, fed = 0, at = len / 3;
    size_t from = 0, dict_end = 0, off, n, dn;
    unsigned char *out = malloc(cap);
    int ret, rk = -100, done = 0, ok;
    unsigned long long h = 14695981039346656037ULL;

    init(&s, level, wbits, threads);
    while (!done) {
        size_t piece = len - fed < size ? len - fed : size;
        int flush = fed + piece == len ? Z_FINISH : Z_NO_FLUSH;
        s.next_in = (unsigned char *)in + fed; s.avail_in = (uInt)piece;
        do {
            s.next_out = out + got; s.avail_out = 4096;
            ret = deflate(&s, flush);
            got = (size_t)(s.next_out - out);
            h = (h ^ (unsigned)(ret + 10)) * 1099511628211ULL;
            if (ret == Z_STREAM_END) done = 1;
        } while (s.avail_out == 0 && !done);
        fed += piece - s.avail_in;
        if (rk == -100 && fed > at) {
            /* the window holds the input up to the lookahead */
            dict_end = fed - ((deflate_state *)s.state)->lookahead;
            rk = deflateResetKeep(&s);
            mark = got;
            from = fed;         /* the input after the reset */
        }
    }
    off = payload(out + mark, got - mark, wbits, &n);
    dn = dict_end < 32768 ? dict_end : 32768;
    ok = inflates_to(out + mark + off, n, in + dict_end - dn, dn,
                     in + from, len - from);
    printf("reset level %d wbits %d threads %d pieces %lu: keep %d end %d "
           "dropped %lu before %lu after %lu hash %016llx round trip %s\n",
           level, wbits, threads, (unsigned long)size, rk, ret,
           (unsigned long)(from - dict_end), (unsigned long)mark,
           (unsigned long)(got - mark), fnv(h, out, got), ok ? "ok" : "FAIL");
#ifdef TURBO
    fails += !ok;
#endif
    deflateEnd(&s);
    free(out);
}

/* deflate(Z_FINISH) into exactly the space needed, in one call. */
static void exact_case(const unsigned char *in, size_t len, int level, int wbits,
                       int threads) {
    z_stream s;
    size_t cap = deflateBound(Z_NULL, (uLong)len) + 4096, need;
    unsigned char *out = malloc(cap);
    int ret;

    init(&s, level, wbits, threads);
    s.next_in = (unsigned char *)in; s.avail_in = (uInt)len;
    s.next_out = out; s.avail_out = (uInt)cap;
    ret = deflate(&s, Z_FINISH);
    if (ret != Z_STREAM_END) { printf("exact first fail %d\n", ret); exit(1); }
    need = s.total_out;
    deflateEnd(&s);
    init(&s, level, wbits, threads);
    s.next_in = (unsigned char *)in; s.avail_in = (uInt)len;
    s.next_out = out; s.avail_out = (uInt)need;
    ret = deflate(&s, Z_FINISH);
    printf("exact level %d wbits %d threads %d: %lu bytes, %d, avail_out %u, "
           "hash %016llx\n", level, wbits, threads, (unsigned long)need, ret,
           s.avail_out, fnv(14695981039346656037ULL, out, need));
    if (level > 9 && ret != Z_STREAM_END) {
        printf("exact level %d: FAIL, not Z_STREAM_END\n", level);
        fails++;
    }
    deflateEnd(&s);
    free(out);
}

#ifdef MORE
/* deflateCopy() at levels 10 to 12 before compressing, the copy to another
   level, then compress with 1000 bytes of output space a call. */
static void copy_case(const unsigned char *in, size_t len, int level, int to) {
    z_stream s, c;
    size_t cap = deflateBound(Z_NULL, (uLong)len) + 4096, got = 0, off, n;
    unsigned char *out = malloc(cap);
    int ret, ok;

    init(&s, level, 15, 1);
    memset(&c, 0, sizeof(c));
    if (deflateCopy(&c, &s) != Z_OK) { printf("copy fail\n"); exit(1); }
    deflateEnd(&s);
    if (deflateParams(&c, to, Z_DEFAULT_STRATEGY) != Z_OK) {
        printf("params fail\n"); exit(1);
    }
    c.next_in = (unsigned char *)in; c.avail_in = (uInt)len;
    do {
        c.next_out = out + got; c.avail_out = 1000;
        ret = deflate(&c, Z_FINISH);
        got = (size_t)(c.next_out - out);
    } while (ret == Z_OK);
    off = payload(out, got, 15, &n);
    ok = ret == Z_STREAM_END && inflates_to(out + off, n, Z_NULL, 0, in, len);
    printf("copy level %d to %d: %lu bytes, round trip %s\n", level, to,
           (unsigned long)got, ok ? "ok" : "FAIL");
    fails += !ok;
    deflateEnd(&c);
    free(out);
}
#endif

int main(int argc, char **argv) {
    static const int wbits[3] = {-15, 15, 31};
    size_t len;
    unsigned char *in;
    int level, w, t;

    if (argc != 2) { printf("usage: zedge FILE\n"); return 1; }
    in = load(argv[1], &len);
    if (len > 3000000) len = 3000000;
    for (level = 0; level <= TOP; level++)
        for (w = 0; w < 3; w++)
            for (t = 1; t <= 4; t += 3) {   /* stock: one thread both times */
                exact_case(in, len < 300000 ? len : 300000, level, wbits[w], t);
                reset_case(in, len, level, wbits[w], t, 65536);
                reset_case(in, len, level, wbits[w], t, 4099);
            }
#ifdef MORE
    for (level = 10; level <= 12; level++) {
        copy_case(in, len < 1000000 ? len : 1000000, level, 0);
        copy_case(in, len < 1000000 ? len : 1000000, level, 1);
        copy_case(in, len < 1000000 ? len : 1000000, level, 6);
        copy_case(in, len < 1000000 ? len : 1000000, level, 9);
    }
#endif
    free(in);
    if (fails) printf("%d FAILED\n", fails);
    return fails != 0;
}
