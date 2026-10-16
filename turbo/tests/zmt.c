/* Copyright (C) 2026 Yingquan (Cody) Wu
 * For conditions of distribution and use, see copyright notice in zlib.h
 */

/* zmt: deflateSetThreads() in zlib-turbo.
   zmt speed LEVEL THREADS FILE...   compress each file whole in one deflate() call: MB/s (best of runs in 1 s, at
                                     least 2) and a hash of all output
   zmt check THREADS FILE...         compress FILE (a prefix of it for the slow cases) with many settings, input
                                     piece sizes, flushes and parameter changes, with 1 and with THREADS threads, and
                                     compare every output byte and return code. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "zlib.h"
#include "zturbo.h"

#ifdef _MSC_VER
static double now(void) { struct timespec t; timespec_get(&t, TIME_UTC); return t.tv_sec + t.tv_nsec * 1e-9; }
#else
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }
#endif

static unsigned char *load(const char *name, size_t *n) {
    FILE *f = fopen(name, "rb");
    unsigned char *p;
    if (!f) { perror(name); exit(1); }
    fseek(f, 0, SEEK_END); *n = (size_t)ftell(f); fseek(f, 0, SEEK_SET);
    p = malloc(*n + 1);
    if (fread(p, 1, *n, f) != *n) exit(1);
    fclose(f);
    return p;
}

typedef struct {
    int level, wbits, mem, strategy;
    size_t piece;       /* input piece size, 0 for all at once */
    size_t outpiece;    /* output space per call, 0 for all */
    size_t flush_every; /* Z_SYNC_FLUSH after this much input, 0 for none */
    int full_every;     /* use Z_FULL_FLUSH instead */
    int param_at;       /* change the level to param_level after this many pieces, 0 for never */
    int param_level;
    int param2_at;      /* and back to level after this many, 0 for never */
    int set_strategy;   /* the first change also sets the strategy to new_strategy */
    int new_strategy;
    int dict;           /* deflateSetDictionary with the first 32 KiB before the data */
    int dict_at;        /* raw only, with a flush every piece: deflateSetDictionary with the first dict_len bytes */
    unsigned dict_len;  /* after this many pieces */
    int bounce;         /* before each deflate() call, move the input not consumed to another buffer and overwrite
                           the old one (needs piece) */
} cfg;

/* deflateParams() with room for the flush it does; returns the hash updated with its return code */
static unsigned long long params(z_stream *s, int level, int strategy, unsigned char **out, size_t *total,
                                 size_t *cap, unsigned long long h) {
    int pr;
    do {
        if (*total + 65536 > *cap) { *cap = (*total + 65536) * 2; *out = realloc(*out, *cap); }
        s->next_out = *out + *total; s->avail_out = 65536;
        pr = deflateParams(s, level, strategy);
        *total += 65536 - s->avail_out;
    } while (pr == Z_BUF_ERROR && s->avail_out == 0);
    return (h ^ (unsigned)(pr + 20)) * 1099511628211ULL;
}

/* compress p[0..n) with c on threads; return a hash of output bytes and return codes, and the size in *out */
static unsigned long long run(const cfg *c, int threads, const unsigned char *p, size_t n, const unsigned char *dict,
                              size_t *outn) {
    z_stream s;
    unsigned long long h = 1469598103934665603ULL;
    size_t pos = 0, since = 0, cap = n + n / 8 + 4096, total = 0;
    unsigned char *out = malloc(cap);
    unsigned char *bounce[2] = {NULL, NULL};
    int r = Z_OK, pieces = 0, finished = 0, bi = 0;
    if (c->bounce) { bounce[0] = malloc(c->piece); bounce[1] = malloc(c->piece); }
    memset(&s, 0, sizeof s);
    if (deflateInit2(&s, c->level, Z_DEFLATED, c->wbits, c->mem, c->strategy) != Z_OK) exit(3);
    if (deflateSetThreads(&s, threads) != Z_OK) exit(4);
    if (c->dict) deflateSetDictionary(&s, dict, 32768);
    while (!finished) {
        size_t k = c->piece ? (n - pos < c->piece ? n - pos : c->piece) : n - pos;
        int flush = Z_NO_FLUSH;
        s.next_in = (unsigned char *)p + pos;
        s.avail_in = (uInt)k;
        pos += k;
        since += k;
        if (pos == n) flush = Z_FINISH;
        else if (c->flush_every && since >= c->flush_every) { flush = c->full_every ? Z_FULL_FLUSH : Z_SYNC_FLUSH; since = 0; }
        do {
            size_t room = c->outpiece ? c->outpiece : cap - total;
            if (total + room > cap) { cap = (total + room) * 2; out = realloc(out, cap); }
            s.next_out = out + total;
            s.avail_out = (uInt)room;
            if (c->bounce) {
                /* the input not consumed moves to the other buffer, and the old one is overwritten */
                memcpy(bounce[bi], s.next_in, s.avail_in);
                memset(bounce[bi ^ 1], 0xa5, c->piece);
                s.next_in = bounce[bi];
                bi ^= 1;
            }
            r = deflate(&s, flush);
            total += room - s.avail_out;
            h = (h ^ (unsigned)(r + 10)) * 1099511628211ULL;
            if (r == Z_STREAM_END) { finished = 1; break; }
            if (r != Z_OK && r != Z_BUF_ERROR) { printf("deflate error %d\n", r); exit(5); }
        } while (s.avail_out == 0 || s.avail_in != 0);
        pieces++;
        if (c->param_at && pieces == c->param_at && !finished)
            h = params(&s, c->param_level, c->set_strategy ? c->new_strategy : c->strategy, &out, &total, &cap, h);
        if (c->param2_at && pieces == c->param2_at && !finished)
            h = params(&s, c->level, c->set_strategy ? c->new_strategy : c->strategy, &out, &total, &cap, h);
        if (c->dict_at && pieces == c->dict_at && !finished)
            h = (h ^ (unsigned)(deflateSetDictionary(&s, dict, c->dict_len) + 30)) * 1099511628211ULL;
    }
    deflateEnd(&s);
    free(bounce[0]); free(bounce[1]);
    for (pos = 0; pos < total; pos++) h = (h ^ out[pos]) * 1099511628211ULL;
    *outn = total;
    free(out);
    return h;
}

int main(int argc, char **argv) {
    if (argc >= 5 && !strcmp(argv[1], "speed")) {
        int level = atoi(argv[2]), threads = atoi(argv[3]), i;
        double tin = 0, tt = 0;
        unsigned long long hall = 1469598103934665603ULL;
        for (i = 4; i < argc; i++) {
            size_t n, cap, outn = 0;
            unsigned char *p = load(argv[i], &n), *out;
            double best = 1e30, total = 0;
            int reps = 0;
            cap = deflateBound(NULL, n) + 64;
            out = malloc(cap);
            do {
                z_stream s;
                double t0 = now(), t;
                memset(&s, 0, sizeof s);
                deflateInit(&s, level);
                deflateSetThreads(&s, threads);
                s.next_in = p; s.avail_in = (uInt)n; s.next_out = out; s.avail_out = (uInt)cap;
                if (deflate(&s, Z_FINISH) != Z_STREAM_END) { printf("%s: FAILED\n", argv[i]); return 2; }
                outn = s.total_out;
                deflateEnd(&s);
                t = now() - t0;
                if (t < best) best = t;
                total += t; reps++;
            } while (total < 1.0 || reps < 2);
            for (cap = 0; cap < outn; cap++) hall = (hall ^ out[cap]) * 1099511628211ULL;
            tin += (double)n; tt += best;
            free(p); free(out);
        }
        printf("TOTAL L%d T%d comp %.2f MB/s hash %016llx\n", level, threads, tin / tt / 1e6, hall);
        return 0;
    }
    if (argc >= 4 && !strcmp(argv[1], "check")) {
        int threads = atoi(argv[2]), a, cases = 0, bad = 0;
        for (a = 3; a < argc; a++) {
            size_t n, small;
            unsigned char *p = load(argv[a], &n);
            int level, k;
            small = n < 3000000 ? n : 3000000;
            for (level = 4; level <= 9; level++) {
                cfg list[24];
                int m = 0;
                memset(list, 0, sizeof list);
                list[m].level = level; list[m].wbits = 15; list[m].mem = 8; m++;                                   /* whole */
                list[m] = list[0]; list[m].piece = 65536; m++;                                                     /* pieces */
                list[m] = list[0]; list[m].piece = 1000003; list[m].outpiece = 4096; m++;                          /* small output */
                list[m] = list[0]; list[m].piece = 300000; list[m].flush_every = 100000; m++;                      /* sync flushes */
                list[m] = list[0]; list[m].piece = 300000; list[m].flush_every = 600000; list[m].full_every = 1; m++; /* full flushes */
                list[m] = list[0]; list[m].strategy = Z_FILTERED; m++;
                list[m] = list[0]; list[m].wbits = 10; list[m].mem = 3; m++;
                list[m] = list[0]; list[m].wbits = -12; list[m].mem = 9; list[m].piece = 777777; m++;              /* raw */
                list[m] = list[0]; list[m].wbits = 31; m++;                                                        /* gzip */
                list[m] = list[0]; list[m].piece = 500000; list[m].param_at = 2; list[m].param_level = level == 9 ? 4 : 9; m++;
                list[m] = list[0]; list[m].piece = 500000; list[m].param_at = 1; list[m].param_level = 1; m++;    /* to deflate_fast and back? once */
                list[m] = list[0]; list[m].dict = 1; m++;
                /* modes that insert fewer positions, and back */
                list[m] = list[0]; list[m].piece = 300000; list[m].param_at = 1; list[m].param_level = 1; list[m].param2_at = 3; m++;
                list[m] = list[0]; list[m].piece = 300000; list[m].param_at = 1; list[m].param_level = 3; list[m].param2_at = 2; m++;
                list[m] = list[0]; list[m].piece = 300000; list[m].param_at = 1; list[m].param_level = 0; list[m].param2_at = 2; m++;
                list[m] = list[0]; list[m].piece = 300000; list[m].strategy = Z_RLE; list[m].param_at = 1;
                list[m].param_level = level; list[m].set_strategy = 1; list[m].new_strategy = Z_DEFAULT_STRATEGY; m++;
                list[m] = list[0]; list[m].piece = 300000; list[m].strategy = Z_HUFFMAN_ONLY; list[m].param_at = 2;
                list[m].param_level = level; list[m].set_strategy = 1; list[m].new_strategy = Z_FILTERED; m++;
                /* frequent full flushes, and full flushes with output space running out */
                list[m] = list[0]; list[m].piece = 50000; list[m].flush_every = 100000; list[m].full_every = 1; m++;
                list[m] = list[0]; list[m].piece = 400000; list[m].flush_every = 400000; list[m].full_every = 1;
                list[m].outpiece = 1000; m++;
                /* raw stream with a dictionary set after data: the window starts again, and a short one */
                list[m] = list[0]; list[m].wbits = -15; list[m].piece = 400000; list[m].flush_every = 400000;
                list[m].dict_at = 2; list[m].dict_len = 40000; m++;
                list[m] = list[0]; list[m].wbits = -15; list[m].piece = 400000; list[m].flush_every = 400000;
                list[m].dict_at = 1; list[m].dict_len = 5000; m++;
                /* the application moves its input between calls */
                list[m] = list[0]; list[m].piece = 2999999; list[m].outpiece = 4096; list[m].bounce = 1; m++;
                list[m] = list[0]; list[m].piece = 2500001; list[m].outpiece = 65536; list[m].bounce = 1; m++;
                for (k = 0; k < m; k++) {
                    size_t len = (k == 0 || k == 1) ? n : small, o1, o2;
                    const unsigned char *data = p, *dict = p;
                    unsigned long long h1, h2;
                    if (list[k].dict) { data = p + 32768; if (len > n - 32768) len = n - 32768; }
                    h1 = run(list + k, 1, data, len, dict, &o1);
                    h2 = run(list + k, threads, data, len, dict, &o2);
                    cases++;
                    if (h1 != h2) {
                        bad++;
                        printf("DIFF %s level %d case %d (piece %zu out %zu flush %zu full %d strat %d wbits %d mem %d param %d->%d dict %d): %zu vs %zu bytes\n",
                               argv[a], level, k, list[k].piece, list[k].outpiece, list[k].flush_every, list[k].full_every,
                               list[k].strategy, list[k].wbits, list[k].mem, list[k].param_at, list[k].param_level, list[k].dict, o1, o2);
                    }
                }
            }
            free(p);
        }
        printf("%d cases, %d differ (threads %d)\n", cases, bad, threads);
        return bad != 0;
    }
    fprintf(stderr, "usage: zmt speed LEVEL THREADS FILE... | zmt check THREADS FILE...\n");
    return 1;
}
