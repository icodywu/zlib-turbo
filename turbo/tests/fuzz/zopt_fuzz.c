/* libFuzzer target: zlib-turbo's levels 10 to 12 (linked in, with ASAN or UBSAN). Every stream must decompress
   with stock zlib's inflate (a shared library opened with dlopen, not instrumented) to the input, and a stream
   made in one deflate() call must fit in deflateBound().
   Input: byte 0 level, 1 window bits choice, 2 memLevel, 3 strategy, 4 flags (bit 0 dictionary, bit 1
   deflateParams to another of levels 10 to 12 after the first flush, bit 2 expand the payload into a long,
   repetitive input, bit 3 deflateCopy mid-stream, bit 4 deflateReset and a second stream), 5 the new level and
   the expansion, then n + input piece sizes, n + flush modes, n + output piece sizes, then the payload. STOCK_SO
   names the library. */
#define _GNU_SOURCE
#ifndef ZOPT_FUZZ_LOCAL
#  include <dlfcn.h>
#endif
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "zlib.h"

static int (*s_inflateInit2_)(z_streamp, int, const char *, int);
static int (*s_inflate)(z_streamp, int);
static int (*s_inflateEnd)(z_streamp);
static int (*s_inflateSetDictionary)(z_streamp, const Bytef *, uInt);
static const char *(*s_zlibVersion)(void);

#ifdef ZOPT_FUZZ_LOCAL
/* Without libFuzzer and dlopen (-DZOPT_FUZZ_LOCAL): this library's inflate, and a main() that runs the inputs
   given as files. */
static void load_stock(void)
{
    s_inflateInit2_ = inflateInit2_; s_inflate = inflate; s_inflateEnd = inflateEnd;
    s_inflateSetDictionary = inflateSetDictionary; s_zlibVersion = zlibVersion;
}
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);
int main(int argc, char **argv)
{
    static unsigned char buf[1 << 20];
    int a;
    for (a = 1; a < argc; a++) {
        FILE *f = fopen(argv[a], "rb");
        size_t n;
        if (f == NULL) { perror(argv[a]); return 1; }
        n = fread(buf, 1, sizeof(buf), f);
        fclose(f);
        LLVMFuzzerTestOneInput(buf, n);
        printf("%s: ok\n", argv[a]);
    }
    return 0;
}
#else
static void load_stock(void)
{
    void *h = dlopen(STOCK_SO, RTLD_NOW | RTLD_LOCAL);
    if (!h) { fprintf(stderr, "dlopen: %s\n", dlerror()); exit(1); }
#define GET(f) *(void **)&s_##f = dlsym(h, #f); if (!s_##f) { fprintf(stderr, "no %s\n", #f); exit(1); }
    GET(inflateInit2_) GET(inflate) GET(inflateEnd) GET(inflateSetDictionary) GET(zlibVersion)
    fprintf(stderr, "stock %s, turbo %s\n", s_zlibVersion(), zlibVersion());
}
#endif

static void fail(const char *what, int a, int b)
{
    fprintf(stderr, "MISMATCH: %s (%d, %d)\n", what, a, b);
    abort();
}

typedef struct {
    int level, wbits, memlevel, strategy, flags, level2;
    const unsigned char *isz, *fl, *osz;
    size_t nisz, nfl, nosz;
} Plan;

static const int wbits_list[8] = { 15, 9, 12, -15, 31, -9, 25, 14 };
static const int flush_list[5] = { Z_NO_FLUSH, Z_SYNC_FLUSH, Z_FULL_FLUSH, Z_PARTIAL_FLUSH, Z_BLOCK };

/* Compress in[0..len) as the plan says; returns the stream's length in out (cap bytes). */
static size_t compress_plan(const Plan *p, const unsigned char *in, size_t len, unsigned char *out, size_t cap,
                            const unsigned char *dict, unsigned dlen, int second)
{
    z_stream s, c;
    size_t done = 0, n, step = 0, ostep = 0;
    int ret, flush, switched = 0, copied = 0;

    memset(&s, 0, sizeof(s));
    ret = deflateInit2(&s, p->level, Z_DEFLATED, p->wbits, p->memlevel, p->strategy);
    if (ret != Z_OK) fail("deflateInit2", ret, Z_OK);
    if (second) {
        s.next_in = (unsigned char *)in; s.avail_in = (uInt)(len < 100 ? len : 100);
        s.next_out = out; s.avail_out = (uInt)cap;
        deflate(&s, Z_NO_FLUSH);
        if (deflateReset(&s) != Z_OK) fail("deflateReset", 0, 0);
    }
    if (dlen && (ret = deflateSetDictionary(&s, dict, dlen)) != Z_OK) fail("deflateSetDictionary", ret, Z_OK);
    s.next_out = out;
    while (done < len) {
        n = p->nisz ? 1 + (size_t)p->isz[step % p->nisz] * ((p->isz[step % p->nisz] & 1) ? 1 : 97) : len;
        if (n > len - done) n = len - done;
        flush = p->nfl ? flush_list[p->fl[step % p->nfl] % 5] : Z_NO_FLUSH;
        if (done + n == len) flush = Z_FINISH;
        s.next_in = (unsigned char *)in + done;
        s.avail_in = (uInt)n;
        do {
            size_t room = cap - (size_t)(s.next_out - out);
            if (p->nosz) {
                size_t o = 1 + (size_t)p->osz[ostep++ % p->nosz] * 7;
                if (flush != Z_NO_FLUSH && o < 8)
                    o = 8;      /* zlib.h: avail_out must be greater than six when flushing, or the markers repeat */
                if (len > 65536 && o < 256)
                    o = 256;    /* long inputs in tiny pieces only test the harness's patience */
                if (o < room) room = o;
            }
            s.avail_out = (uInt)room;
            ret = deflate(&s, flush);
            if (ret == Z_STREAM_ERROR) fail("deflate", ret, flush);
        } while (s.avail_out == 0 || s.avail_in != 0 || (flush == Z_FINISH && ret != Z_STREAM_END));
        done += n;
        step++;
        if (flush != Z_NO_FLUSH && flush != Z_FINISH && (p->flags & 2) && !switched) {
            switched = 1;
            ret = deflateParams(&s, p->level2, p->strategy);
            if (ret != Z_OK) fail("deflateParams", ret, p->level2);
        }
        if ((p->flags & 8) && !copied && done >= len / 2 && done < len) {
            copied = 1;
            if (deflateCopy(&c, &s) != Z_OK) fail("deflateCopy", 0, 0);
            deflateEnd(&s);
            if (deflateCopy(&s, &c) != Z_OK) fail("deflateCopy", 1, 0);
            deflateEnd(&c);
        }
    }
    if (len == 0) {
        s.avail_in = 0;
        do {
            s.avail_out = (uInt)(cap - (size_t)(s.next_out - out));
            ret = deflate(&s, Z_FINISH);
        } while (ret == Z_OK);
        if (ret != Z_STREAM_END) fail("deflate finish", ret, 0);
    }
    n = (size_t)(s.next_out - out);
    deflateEnd(&s);
    return n;
}

static void check(const Plan *p, const unsigned char *z, size_t zn, const unsigned char *in, size_t len,
                  const unsigned char *dict, unsigned dlen)
{
    z_stream s;
    unsigned char *o = malloc(len + 1);
    int ret;

    memset(&s, 0, sizeof(s));
    if (s_inflateInit2_(&s, p->wbits, ZLIB_VERSION, (int)sizeof(z_stream)) != Z_OK)
        fail("inflateInit2", 0, 0);
    if (dlen && p->wbits < 0) s_inflateSetDictionary(&s, dict, dlen);
    s.next_in = (unsigned char *)z; s.avail_in = (uInt)zn;
    s.next_out = o; s.avail_out = (uInt)len + 1;
    ret = s_inflate(&s, Z_FINISH);
    if (ret == Z_NEED_DICT) {
        s_inflateSetDictionary(&s, dict, dlen);
        ret = s_inflate(&s, Z_FINISH);
    }
    if (ret != Z_STREAM_END) fail("stock inflate", ret, (int)s.total_out);
    if (s.total_out != len || memcmp(o, in, len)) fail("round trip", (int)s.total_out, (int)len);
    if (s.avail_in != 0) fail("trailing bytes", (int)s.avail_in, 0);
    s_inflateEnd(&s);
    free(o);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    static int loaded;
    Plan p;
    size_t pos = 6, len, cap, zn;
    unsigned char *in, *out;
    unsigned dlen = 0;

    if (!loaded) { load_stock(); loaded = 1; }
    if (size < 9) return 0;
    p.level = 10 + data[0] % 3;
    p.wbits = wbits_list[data[1] % 8];
    p.memlevel = 1 + data[2] % 9;
    p.strategy = data[3] % 5;
    p.flags = data[4];
    p.level2 = 10 + data[5] % 3;
    p.nisz = data[pos++] % 8; p.isz = data + pos; pos += p.nisz;
    if (pos >= size) return 0;
    p.nfl = data[pos++] % 8; p.fl = data + pos; pos += p.nfl;
    if (pos >= size) return 0;
    p.nosz = data[pos++] % 8; p.osz = data + pos; pos += p.nosz;
    if (pos > size) return 0;
    len = size - pos;
    if (p.flags & 4) {          /* long and repetitive: segments, long matches, many blocks */
        size_t k, m = len ? len * (2 + data[5] % 40) : 0, j;
        if (m > 1200000) m = 1200000;
        in = malloc(m + 1);
        for (k = 0, j = 0; k < m; k++, j++) {
            if (j == len) j = 0;
            in[k] = (unsigned char)(data[pos + j] + (k % 7919 == 0 ? k / 7919 : 0));
        }
        len = m;
    }
    else {
        in = malloc(len + 1);
        memcpy(in, data + pos, len);
    }
    if ((p.flags & 1) && len > 2) dlen = (unsigned)(len / 3 > 40000 ? 40000 : len / 3);
    if (p.wbits == 31 || p.wbits == 25) dlen = 0;   /* no dictionary for gzip */
    cap = 16 * len + 70000;             /* also for a flush after every byte */
    out = malloc(cap);
    zn = compress_plan(&p, in, len, out, cap, in, dlen, (p.flags & 16) != 0);
    check(&p, out, zn, in, len, in, dlen);
    if (p.nisz == 0 && p.nfl == 0 && !(p.flags & 2) && !(p.flags & 16) && dlen == 0) {
        z_stream s;
        uLong bound;
        memset(&s, 0, sizeof(s));
        deflateInit2(&s, p.level, Z_DEFLATED, p.wbits, p.memlevel, p.strategy);
        bound = deflateBound(&s, (uLong)len);
        deflateEnd(&s);
        if (zn > bound) fail("deflateBound", (int)zn, (int)bound);
    }
    free(in);
    free(out);
    return 0;
}
