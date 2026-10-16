/* Copyright (C) 2026 Yingquan (Cody) Wu
 * For conditions of distribution and use, see copyright notice in zlib.h
 */

/* zdict: dictionaries in zlib and zlib-turbo.
   zdict check FILE   compresses pieces of FILE with dictionaries from FILE, over levels, window bits, memLevels,
                      strategies and dictionary lengths, through deflateSetDictionary() (path A) and, with zlib-turbo,
                      deflateUseDictionary() (path B). Prints a hash of everything each path produced (output bytes,
                      return codes, total_in, adler): stock A, turbo A and turbo B must all agree.
   zdict bench FILE LEVEL MSGSIZE
                      compresses FILE in MSGSIZE pieces, each as its own zlib stream after deflateReset() with a 32 KiB
                      dictionary taken from the start of FILE: µs per message and MB/s for path A, and B with turbo. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "zlib.h"
#ifdef ZLIB_TURBO_BUILD
#include "zturbo.h"
#endif

#ifdef _WIN32
#include <windows.h>
static double now(void) { LARGE_INTEGER f, t; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t); return (double)t.QuadPart / (double)f.QuadPart; }
#else
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }
#endif
static unsigned long long H = 1469598103934665603ULL;
static void mix(const void *p, size_t n) { const unsigned char *q = p; while (n--) H = (H ^ *q++) * 1099511628211ULL; }
static void mixi(long v) { mix(&v, sizeof v); }

static unsigned char *load(const char *name, size_t *n) {
    FILE *f = fopen(name, "rb"); unsigned char *p;
    if (!f) { perror(name); exit(1); }
    fseek(f, 0, SEEK_END); *n = (size_t)ftell(f); fseek(f, 0, SEEK_SET);
    p = malloc(*n + 1);
    if (fread(p, 1, *n, f) != *n) exit(1);
    fclose(f);
    return p;
}

/* compress msg on s (dictionary already set), in one call, into out; hash the result */
static void finish(z_stream *s, const unsigned char *msg, size_t m, unsigned char *out, size_t cap) {
    int r;
    s->next_in = (unsigned char *)msg; s->avail_in = (uInt)m;
    s->next_out = out; s->avail_out = (uInt)cap;
    r = deflate(s, Z_FINISH);
    mixi(r); mixi((long)s->total_in); mixi((long)s->total_out); mixi((long)s->adler);
    mix(out, cap - s->avail_out);
}

int main(int argc, char **argv) {
    size_t n, cap;
    unsigned char *p, *out;
    if (argc < 3) return 1;
    p = load(argv[2], &n);
    cap = 1 << 20; out = malloc(cap);
    if (!strcmp(argv[1], "check")) {
        static const int wbits[5] = { 15, 12, 9, -15, -10 };
        static const int mems[3] = { 8, 1, 9 };
        static const unsigned dlens[9] = { 0, 1, 2, 3, 100, 4000, 32768, 40000, 70000 };
        unsigned long long ha, hb = 0;
        int lev, wi, mi, st, di, k, cases = 0;
#ifdef ZLIB_TURBO_BUILD
        int pathb;
        for (pathb = 0; pathb < 2; pathb++)
#endif
        {
            H = 1469598103934665603ULL;
            for (lev = 0; lev <= 9; lev++) for (wi = 0; wi < 5; wi++) for (mi = 0; mi < 3; mi++)
            for (st = 0; st < 5; st += (lev == 6 ? 1 : 4)) for (di = 0; di < 9; di++) for (k = 0; k < 3; k++) {
                z_stream s;
                size_t doff = ((size_t)k * 7919 * 97 + di * 1031) % (n - 80000);
                size_t moff = (doff + dlens[di] + (size_t)k * 13) % (n - 70000);
                size_t m = k == 0 ? 300 : k == 1 ? 5000 : 66000;
                int r;
                memset(&s, 0, sizeof s);
                if (deflateInit2(&s, lev, Z_DEFLATED, wbits[wi], mems[mi], st) != Z_OK) { mixi(-1000); continue; }
#ifdef ZLIB_TURBO_BUILD
                if (pathb) {
                    /* prepare on a scratch stream with the same parameters, use on s */
                    z_stream t; z_dict *d = NULL;
                    memset(&t, 0, sizeof t);
                    deflateInit2(&t, lev, Z_DEFLATED, wbits[wi], mems[mi], st);
                    r = deflatePrepareDictionary(&t, p + doff, dlens[di], &d);
                    deflateEnd(&t);
                    if (r == Z_OK) { r = deflateUseDictionary(&s, d); deflateFreeDictionary(d); }
                    else r = deflateSetDictionary(&s, p + doff, dlens[di]);
                } else
#endif
                r = deflateSetDictionary(&s, p + doff, dlens[di]);
                mixi(r); mixi((long)s.total_in); mixi((long)s.adler);
                finish(&s, p + moff, m, out, cap);
                /* a second message on the same stream after deflateReset */
                deflateReset(&s);
#ifdef ZLIB_TURBO_BUILD
                if (pathb) {
                    z_stream t; z_dict *d = NULL;
                    memset(&t, 0, sizeof t);
                    deflateInit2(&t, lev, Z_DEFLATED, wbits[wi], mems[mi], st);
                    r = deflatePrepareDictionary(&t, p + moff, dlens[di], &d);
                    deflateEnd(&t);
                    if (r == Z_OK) { r = deflateUseDictionary(&s, d); deflateFreeDictionary(d); }
                    else r = deflateSetDictionary(&s, p + moff, dlens[di]);
                } else
#endif
                r = deflateSetDictionary(&s, p + moff, dlens[di]);
                mixi(r);
                finish(&s, p + doff, m / 3 + 1, out, cap);
                deflateEnd(&s);
                cases++;
            }
#ifdef ZLIB_TURBO_BUILD
            if (pathb) hb = H; else
#endif
            ha = H;
        }
        printf("%s: %d cases, path A %016llx, path B %016llx\n", zlibVersion(), cases, ha, hb);
        return 0;
    }
    if (!strcmp(argv[1], "bench")) {
        int level = atoi(argv[3]);
        size_t msg = (size_t)atol(argv[4]), dlen = 32768, start = dlen, i, nmsg = 0;
        double best[2] = { 1e30, 1e30 };
        int path, rep;
        z_stream s;
        memset(&s, 0, sizeof s);
        deflateInit2(&s, level, Z_DEFLATED, 15, 8, Z_DEFAULT_STRATEGY);
#ifdef ZLIB_TURBO_BUILD
        z_dict *d = NULL;
        {   z_stream t; memset(&t, 0, sizeof t);
            deflateInit2(&t, level, Z_DEFLATED, 15, 8, Z_DEFAULT_STRATEGY);
            if (deflatePrepareDictionary(&t, p, (uInt)dlen, &d) != Z_OK) return 2;
            deflateEnd(&t); }
#endif
        for (rep = 0; rep < 5; rep++) for (path = 0; path < 2; path++) {
            double t0;
            unsigned long long h0 = H;
#ifndef ZLIB_TURBO_BUILD
            if (path) continue;
#endif
            nmsg = 0;
            t0 = now();
            for (i = start; i + msg <= n && nmsg < 20000; i += msg, nmsg++) {
                deflateReset(&s);
#ifdef ZLIB_TURBO_BUILD
                if (path) deflateUseDictionary(&s, d); else
#endif
                deflateSetDictionary(&s, p, (uInt)dlen);
                finish(&s, p + i, msg, out, cap);
            }
            t0 = now() - t0;
            if (t0 < best[path]) best[path] = t0;
            (void)h0;
        }
        printf("L%d msg %5zu B x %zu: setDictionary %.2f us/msg (%.1f MB/s)", level, msg, nmsg,
               best[0] / nmsg * 1e6, nmsg * msg / best[0] / 1e6);
#ifdef ZLIB_TURBO_BUILD
        printf(" | useDictionary %.2f us/msg (%.1f MB/s)", best[1] / nmsg * 1e6, nmsg * msg / best[1] / 1e6);
#endif
        printf("\n");
        return 0;
    }
    return 1;
}
