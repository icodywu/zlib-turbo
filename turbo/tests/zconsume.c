/* Copyright (C) 2026 Yingquan (Cody) Wu
 * For conditions of distribution and use, see copyright notice in zlib.h
 */

/* zconsume: the input inflate() uses, on the linked zlib (stock or patched).
   usage: zconsume file...
   Each file (its first 4 MB) is compressed at levels 1, 6 and 9 as a raw, zlib and gzip stream, 100 other bytes are
   put after each stream, and the stream is decoded with input pieces of 1, 7, 100, 10000 bytes or all of it and
   output pieces of 1, 300, 65536 bytes or all of it (pieces of one byte only for files up to 300 KB). One line per
   decode: the return code, total_in, total_out, the input left and used, the CRC-32 of the output and a hash of the
   return code, avail_in and avail_out after every inflate() call. The logs of two builds must be identical: a
   program that finds where a stream ends from avail_in, such as a zip reader, then sees the same. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "zlib.h"

static unsigned char *slurp(const char *f, size_t *n) {
    FILE *fp = fopen(f, "rb");
    long m;
    unsigned char *p;
    if (!fp) return NULL;
    fseek(fp, 0, SEEK_END); m = ftell(fp); fseek(fp, 0, SEEK_SET);
    p = malloc(m + 1); *n = fread(p, 1, m, fp); fclose(fp);
    return p;
}

int main(int argc, char **argv) {
    static const int lev[] = { 1, 6, 9 }, wb[] = { -15, 15, 31 };
    static const size_t ich[] = { 1, 7, 100, 10000, (size_t)-1 }, och[] = { 1, 300, 65536, (size_t)-1 };
    unsigned long calls = 0;
    for (int a = 1; a < argc; a++) {
        size_t n;
        unsigned char *d = slurp(argv[a], &n);
        if (!d) continue;
        if (n > (4u << 20)) n = 4u << 20;
        for (int li = 0; li < 3; li++) for (int wi = 0; wi < 3; wi++) {
            z_stream s;
            memset(&s, 0, sizeof s);
            deflateInit2(&s, lev[li], Z_DEFLATED, wb[wi], 8, Z_DEFAULT_STRATEGY);
            size_t cap = deflateBound(&s, n) + 200;
            unsigned char *c = malloc(cap);
            s.next_in = d; s.avail_in = (uInt)n; s.next_out = c; s.avail_out = (uInt)cap;
            deflate(&s, Z_FINISH);
            size_t cn = s.total_out;
            deflateEnd(&s);
            for (int t = 0; t < 100; t++) c[cn + t] = (unsigned char)(t * 37 + 11);   /* trailing bytes */
            size_t tot = cn + 100;
            unsigned char *o = malloc(n + 1);
            for (int ii = 0; ii < 5; ii++) for (int oi = 0; oi < 4; oi++) {
                if ((ich[ii] == 1 || och[oi] == 1) && n > 300000) continue;
                z_stream z;
                memset(&z, 0, sizeof z);
                inflateInit2(&z, wb[wi]);
                size_t ip = 0, op = 0;
                int r = Z_OK;
                unsigned long h = 0;
                for (;;) {
                    if (z.avail_in == 0 && ip < tot) {
                        size_t m = ich[ii] < tot - ip ? ich[ii] : tot - ip;
                        z.next_in = c + ip; z.avail_in = (uInt)m; ip += m;
                    }
                    size_t w = och[oi] < n + 1 - op ? och[oi] : n + 1 - op;
                    z.next_out = o + op; z.avail_out = (uInt)w;
                    r = inflate(&z, Z_NO_FLUSH);
                    calls++;
                    op += w - z.avail_out;
                    h = h * 1000003u + (unsigned long)(r + 10) * 65537u + z.avail_in * 31u + z.avail_out;
                    if (r != Z_OK) break;
                }
                printf("%s L%d w%d i%zu o%zu r=%d total_in=%lu total_out=%lu left=%u used=%zu crc=%08lx calls=%08lx\n",
                       argv[a], lev[li], wb[wi], ich[ii], och[oi], r, z.total_in, z.total_out, z.avail_in,
                       ip - z.avail_in, crc32(0, o, (uInt)op), h & 0xffffffffUL);
                inflateEnd(&z);
            }
            free(o); free(c);
        }
        free(d);
    }
    fprintf(stderr, "%lu inflate() calls\n", calls);
    return 0;
}
