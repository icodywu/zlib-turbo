/* zopt: tests and speed of zlib-turbo levels 10 to 12.
   zopt ratio LEVEL FILE...   one deflate() call per file: size, MB/s, round trip
   zopt check FILE...         streams with many feeding/flush patterns, all levels
                              10-12: round trip, and the same bytes as one call
                              whenever the flushes are the same
   zopt write LEVEL IN OUT    zlib stream of IN to OUT (for other inflaters) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "zlib.h"
static int cur_level, cur_case;

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

typedef struct {
    size_t piece;       /* input per call (0: all) */
    size_t outpiece;    /* output space per call (0: all) */
    size_t flush_every; /* Z_SYNC_FLUSH (or full) every this many input bytes */
    int full;           /* full flushes instead */
    int wbits, mem, strategy;
    int dict;           /* dictionary of this many bytes from the input */
    int copy;           /* deflateCopy() mid-stream, continue on the copy */
    int switchto;       /* deflateParams() to this level after a flush */
} pat;

/* Compress in[0..len) with pattern p at level; returns size in out. */
static size_t comp(const unsigned char *in, size_t len, int level, const pat *p,
                   unsigned char *out, size_t cap, const unsigned char *dict) {
    z_stream s, c;
    size_t done = 0, nextflush = p->flush_every ? p->flush_every : (size_t)-1;
    int ret, copied = 0, switched = 0;
    memset(&s, 0, sizeof(s));
    if (deflateInit2(&s, level, Z_DEFLATED, p->wbits, p->mem, p->strategy) != Z_OK) {
        printf("init fail\n"); exit(1);
    }
    if (p->dict && deflateSetDictionary(&s, dict, p->dict) != Z_OK) {
        printf("dict fail\n"); exit(1);
    }
    s.next_out = out;
    for (;;) {
        size_t lim = len, n;
        int flush;
        if (nextflush < lim) lim = nextflush;
        n = lim - done;
        if (p->piece && n > p->piece) n = p->piece;
        s.next_in = (unsigned char *)in + done;
        s.avail_in = (uInt)n;
        flush = done + n == len ? Z_FINISH :
                done + n == nextflush ? (p->full ? Z_FULL_FLUSH : Z_SYNC_FLUSH) : Z_NO_FLUSH;
        do {
            size_t room = cap - (s.next_out - out);
            if (room == 0) {
                printf("output buffer full L%d case %d\n", cur_level, cur_case);
                exit(1);
            }
            if (p->outpiece && room > p->outpiece) room = p->outpiece;
            s.avail_out = (uInt)room;
            ret = deflate(&s, flush);
            if (ret == Z_STREAM_ERROR) {
                printf("deflate error L%d case %d flush %d\n", cur_level, cur_case, flush);
                exit(1);
            }
        } while (s.avail_out == 0 || (flush == Z_FINISH && ret != Z_STREAM_END) ||
                 (flush != Z_NO_FLUSH && flush != Z_FINISH && s.avail_in));
        done += n - s.avail_in;
        if (flush == Z_FINISH) break;
        if (flush != Z_NO_FLUSH) {
            nextflush += p->flush_every;
            if (p->switchto && !switched) {
                switched = 1;
                if (deflateParams(&s, p->switchto, p->strategy) != Z_OK) {
                    printf("params fail\n"); exit(1);
                }
            }
        }
        if (p->copy && !copied && done >= len / 2) {
            copied = 1;
            if (deflateCopy(&c, &s) != Z_OK) { printf("copy fail\n"); exit(1); }
            deflateEnd(&s);
            if (deflateCopy(&s, &c) != Z_OK) { printf("copy fail\n"); exit(1); }
            deflateEnd(&c);
        }
    }
    deflateEnd(&s);
    return s.next_out - out;
}

static int roundtrip(const unsigned char *z, size_t zn, const unsigned char *in, size_t len,
                     const pat *p, const unsigned char *dict) {
    z_stream s;
    unsigned char *o = malloc(len + 1);
    int ret, ok;
    memset(&s, 0, sizeof(s));
    inflateInit2(&s, p->wbits);
    s.next_in = (unsigned char *)z;
    s.avail_in = (uInt)zn;
    s.next_out = o;
    s.avail_out = (uInt)len + 1;
    ret = inflate(&s, Z_FINISH);
    if (ret == Z_NEED_DICT && p->dict) {
        inflateSetDictionary(&s, dict, p->dict);
        ret = inflate(&s, Z_FINISH);
    }
    if (p->wbits < 0 && p->dict) {   /* raw: dictionary set before */
        inflateEnd(&s);
        memset(&s, 0, sizeof(s));
        inflateInit2(&s, p->wbits);
        inflateSetDictionary(&s, dict, p->dict);
        s.next_in = (unsigned char *)z; s.avail_in = (uInt)zn;
        s.next_out = o; s.avail_out = (uInt)len + 1;
        ret = inflate(&s, Z_FINISH);
    }
    ok = ret == Z_STREAM_END && s.total_out == len && memcmp(o, in, len) == 0 &&
         s.avail_in == 0;
    inflateEnd(&s);
    free(o);
    return ok;
}

static double now(void) { return (double)clock() / CLOCKS_PER_SEC; }

int main(int argc, char **argv) {
    int a;
    if (argc < 3) { fprintf(stderr, "usage: see source\n"); return 1; }
    if (strcmp(argv[1], "write") == 0) {
        size_t len, cap, n;
        unsigned char *in = load(argv[3], &len), *out;
        pat p;
        FILE *f;
        memset(&p, 0, sizeof(p)); p.wbits = 15; p.mem = 8;
        cap = len + len / 100 + 1000;
        out = malloc(cap);
        n = comp(in, len, atoi(argv[2]), &p, out, cap, NULL);
        f = fopen(argv[4], "wb");
        fwrite(out, 1, n, f);
        fclose(f);
        printf("%s %zu -> %zu\n", argv[3], len, n);
        free(in); free(out);
        return 0;
    }
    if (strcmp(argv[1], "ratio") == 0) {
        int level = atoi(argv[2]);
        size_t tin = 0, tout = 0;
        double tt = 0;
        for (a = 3; a < argc; a++) {
            size_t len, cap, n;
            unsigned char *in = load(argv[a], &len), *out;
            pat p;
            double t;
            memset(&p, 0, sizeof(p)); p.wbits = 15; p.mem = 8;
            cap = len + len / 100 + 1000;
            out = malloc(cap);
            t = now();
            n = comp(in, len, level, &p, out, cap, NULL);
            t = now() - t;
            printf("%-12s L%d %10zu -> %10zu  %6.3f%%  %7.2f MB/s %s\n", argv[a], level, len, n,
                   100.0 * n / len, len / 1e6 / (t > 0 ? t : 1e-9),
                   roundtrip(out, n, in, len, &p, NULL) ? "ok" : "ROUNDTRIP FAIL");
            tin += len; tout += n; tt += t;
            free(in); free(out);
        }
        printf("TOTAL L%d %zu -> %zu %.3f%% %.2f MB/s\n", level, tin, tout, 100.0 * tout / tin,
               tin / 1e6 / tt);
        return 0;
    }
    if (strcmp(argv[1], "check") == 0) {
        static const pat list[] = {
            {0, 0, 0, 0, 15, 8, 0, 0, 0, 0},
            {1, 0, 0, 0, 15, 8, 0, 0, 0, 0},
            {4093, 7, 0, 0, 15, 8, 0, 0, 0, 0},
            {100000, 1, 0, 0, 15, 8, 0, 0, 0, 0},
            {0, 0, 0, 0, -15, 8, 0, 0, 0, 0},
            {0, 0, 0, 0, 31, 8, 0, 0, 0, 0},
            {0, 0, 0, 0, 9, 1, 0, 0, 0, 0},
            {0, 0, 0, 0, 12, 9, 0, 0, 0, 0},
            {0, 0, 0, 0, 15, 8, Z_FIXED, 0, 0, 0},
            {0, 0, 0, 0, 15, 8, Z_FILTERED, 0, 0, 0},
            {0, 0, 0, 0, 15, 8, Z_HUFFMAN_ONLY, 0, 0, 0},
            {0, 0, 0, 0, 15, 8, Z_RLE, 0, 0, 0},
            {0, 0, 70000, 0, 15, 8, 0, 0, 0, 0},
            {333, 9, 70000, 0, 15, 8, 0, 0, 0, 0},
            {0, 0, 100003, 1, 15, 8, 0, 0, 0, 0},
            {0, 0, 0, 0, 15, 8, 0, 20000, 0, 0},
            {0, 0, 0, 0, -15, 8, 0, 40000, 0, 0},
            {0, 0, 50000, 0, -15, 8, 0, 3000, 0, 0},
            {65536, 0, 0, 0, 15, 8, 0, 0, 1, 0},
            {0, 0, 131072, 0, 15, 8, 0, 0, 0, 12},
            {0, 0, 131072, 0, 15, 8, 0, 0, 0, 10},
            {0, 0, 97, 0, 15, 8, 0, 0, 0, 0},
            {13, 64, 31, 1, -15, 8, 0, 0, 0, 0},
        };
        int np = sizeof(list) / sizeof(list[0]), k, level, bad = 0, cases = 0;
        for (a = 2; a < argc; a++) {
            size_t len, cap;
            unsigned char *in = load(argv[a], &len), *out, *ref;
            size_t refn = 0;
            cap = 2 * len + 100000;     /* a flush every 31 bytes of random data: 1.33 times */
            out = malloc(cap);
            ref = malloc(cap);
            for (level = 10; level <= 12; level++)
                for (k = 0; k < np; k++) {
                    pat p = list[k];
                    size_t n;
                    if (p.switchto == level) continue;
                    if ((size_t)p.dict > len / 2) p.dict = (int)(len / 2);
                    cur_level = level; cur_case = k;
                    n = comp(in, len, level, &p, out, cap, in);
                    cases++;
                    if (k == 0) { memcpy(ref, out, n); refn = n; }
                    if (!roundtrip(out, n, in, len, &p, in)) {
                        printf("ROUNDTRIP FAIL %s level %d case %d\n", argv[a], level, k);
                        bad++;
                    }
                    if (k > 0 && k < 4 && (n != refn || memcmp(out, ref, n))) {
                        printf("FEED DIFF %s level %d case %d: %zu vs %zu\n", argv[a], level,
                               k, n, refn);
                        bad++;
                    }
                }
            free(in); free(out); free(ref);
        }
        printf("%d cases, %d bad\n", cases, bad);
        return bad != 0;
    }
    return 1;
}
