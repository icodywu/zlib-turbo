/* zbench: decompression speed of a zlib-API library (stock or patched zlib, or zlib-ng in compat mode) on zlib streams made by
   zmake (file.z holds the stream; file is the original).
   usage: zbench mode file...   mode 0: one call, whole input and output; mode 1: 16 KiB in / 64 KiB out chunks
   (as gzip-style tools feed inflate). Per file: best of the decompressions in at least 1 s, output verified once.
   With ZGZIP set in the environment, the streams are gzip streams (from zmake with ZGZIP set). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "zlib.h"

#ifdef _WIN32
#include <windows.h>
static double now(void) { LARGE_INTEGER f, t; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t); return (double)t.QuadPart / (double)f.QuadPart; }
#else
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }
#endif

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

/* decompress c[0, cn) into o (capacity on); returns the decoded size, or -1 on an error */
static long run(int mode, const unsigned char* c, size_t cn, unsigned char* o, size_t on, unsigned char* tmp)
{
    z_stream s; memset(&s, 0, sizeof s);
    if (inflateInit2(&s, getenv("ZGZIP") ? 31 : 15) != Z_OK) return -1;
    long total = 0;
    int r;
    if (mode == 0) {
        s.next_in = (unsigned char*)c; s.avail_in = (uInt)cn;
        s.next_out = o; s.avail_out = (uInt)on;
        r = inflate(&s, Z_FINISH);
        total = (long)s.total_out;
    } else {
        size_t pos = 0;
        do {
            if (s.avail_in == 0) {
                size_t k = cn - pos < 16384 ? cn - pos : 16384;
                s.next_in = (unsigned char*)c + pos; s.avail_in = (uInt)k; pos += k;
            }
            s.next_out = tmp; s.avail_out = 65536;
            r = inflate(&s, Z_NO_FLUSH);
            size_t got = 65536 - s.avail_out;
            if (o) { if ((size_t)total + got > on) { r = Z_DATA_ERROR; break; } memcpy(o + total, tmp, got); }
            total += (long)got;
        } while (r == Z_OK);
    }
    inflateEnd(&s);
    return r == Z_STREAM_END ? total : -1;
}

int main(int argc, char** argv)
{
    if (argc < 3) { fprintf(stderr, "usage: zbench mode file...\n"); return 1; }
    const int mode = atoi(argv[1]);
    unsigned char* tmp = malloc(65536);
    double tin = 0, tt = 0;
    for (int i = 2; i < argc; i++) {
        char zname[4096]; snprintf(zname, sizeof zname, "%s.z", argv[i]);
        size_t n, cn;
        unsigned char* orig = load(argv[i], &n);
        unsigned char* c = load(zname, &cn);
        unsigned char* o = malloc(n + 1);
        if (run(mode, c, cn, o, n, tmp) != (long)n || memcmp(o, orig, n)) { printf("%s: MISMATCH\n", argv[i]); return 2; }
        double best = 1e30, total = 0;
        int reps = 0;
        do {
            const double t0 = now();
            const long got = run(mode, c, cn, mode == 0 ? o : NULL, n, tmp);
            const double t = now() - t0;
            if (got != (long)n) { printf("%s: FAILED\n", argv[i]); return 2; }
            if (t < best) best = t;
            total += t; reps++;
        } while (total < 1.0 || reps < 3);
        printf("%-12s %10zu -> %10zu  %8.1f MB/s\n", strrchr(argv[i], '/') ? strrchr(argv[i], '/') + 1 : argv[i], n, cn, n / best / 1e6);
        tin += (double)n; tt += best;
        free(orig); free(c); free(o);
    }
    printf("TOTAL %s %.1f MB/s (%s, mode %d)\n", zlibVersion(), tin / tt / 1e6, argv[0], mode);
    return 0;
}
