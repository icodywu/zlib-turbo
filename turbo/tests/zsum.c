/* zsum: crc32_z() and adler32_z() of the linked zlib against plain byte-at-a-time references, so that a build with
   the SIMD checksums (-DZ_TURBO_SIMD) can be checked to give the scalar code's results exactly.
   zsum check     every length 0 to 4096 at every alignment 0 to 63, with fixed and random starting values; long
                  buffers of random bytes and of all ones (the largest Adler-32 sums), in one call and in random
                  pieces; crc32_combine(), crc32_combine_gen()/crc32_combine_op() and adler32_combine()
   zsum speed     crc32_z() and adler32_z() in GB/s on 64 MiB, whole and in 16 KiB pieces (best of 7) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "zlib.h"

static unsigned long crc_table[256];

static void make_crc_table(void) {
    unsigned long c;
    int n, k;
    for (n = 0; n < 256; n++) {
        c = (unsigned long)n;
        for (k = 0; k < 8; k++)
            c = c & 1 ? 0xedb88320UL ^ (c >> 1) : c >> 1;
        crc_table[n] = c;
    }
}

static unsigned long ref_crc(unsigned long crc, const unsigned char *p, size_t n) {
    crc = ~crc & 0xffffffffUL;
    while (n--)
        crc = crc_table[(crc ^ *p++) & 0xff] ^ (crc >> 8);
    return ~crc & 0xffffffffUL;
}

static unsigned long ref_adler(unsigned long adler, const unsigned char *p, size_t n) {
    unsigned long a = adler & 0xffff, b = adler >> 16;
    while (n--) {
        a = (a + *p++) % 65521;
        b = (b + a) % 65521;
    }
    return b << 16 | a;
}

static unsigned long long seed = 0x9e3779b97f4a7c15ULL;
static unsigned long rnd(void) {
    seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
    return (unsigned long)(seed >> 33);
}

static long bad;

static void same(const char *what, unsigned long got, unsigned long want, size_t len, size_t off) {
    if (got != want) {
        if (bad < 20)
            printf("DIFF %s len %lu offset %lu: %08lx, reference %08lx\n", what, (unsigned long)len,
                   (unsigned long)off, got, want);
        bad++;
    }
}

/* the checksum of p[0..n) by the library in random pieces */
static unsigned long pieces(int crc, unsigned long start, const unsigned char *p, size_t n) {
    unsigned long v = start;
    while (n) {
        size_t k = rnd() % 3 == 0 ? rnd() % 100 : rnd() % 300000;
        if (k > n) k = n;
        v = crc ? crc32_z(v, p, k) : adler32_z(v, p, k);
        p += k;
        n -= k;
    }
    return v;
}

static double now(void) { return (double)clock() / CLOCKS_PER_SEC; }

int main(int argc, char **argv) {
    size_t big = 24 << 20, len, off, i;
    unsigned char *buf, *ones;
    long cases = 0;

    if (argc < 2) { fprintf(stderr, "usage: zsum check | zsum speed\n"); return 1; }
    buf = malloc(big + 64);
    ones = malloc(big + 64);
    if (buf == NULL || ones == NULL) return 1;
    for (i = 0; i < big + 64; i++) {
        buf[i] = (unsigned char)(rnd() >> 7);
        ones[i] = 0xff;
    }
    if (strcmp(argv[1], "speed") == 0) {
        size_t n = 64 << 20;
        unsigned char *p = malloc(n);
        double best[4] = {1e9, 1e9, 1e9, 1e9}, t;
        unsigned long sink = 0;
        int r;
        for (i = 0; i < n; i++) p[i] = (unsigned char)(rnd() >> 7);
        for (r = 0; r < 7; r++) {
            t = now(); sink += crc32_z(0, p, n); t = now() - t; if (t < best[0]) best[0] = t;
            t = now(); { unsigned long c = 0; for (i = 0; i < n; i += 16384) c = crc32_z(c, p + i, 16384); sink += c; }
            t = now() - t; if (t < best[1]) best[1] = t;
            t = now(); sink += adler32_z(1, p, n); t = now() - t; if (t < best[2]) best[2] = t;
            t = now(); { unsigned long c = 1; for (i = 0; i < n; i += 16384) c = adler32_z(c, p + i, 16384); sink += c; }
            t = now() - t; if (t < best[3]) best[3] = t;
        }
        printf("crc32 %.2f GB/s whole, %.2f GB/s in 16 KiB pieces; adler32 %.2f GB/s whole, %.2f GB/s in pieces (%lu)\n",
               n / best[0] / 1e9, n / best[1] / 1e9, n / best[2] / 1e9, n / best[3] / 1e9, sink & 1);
        free(p);
        free(buf);
        free(ones);
        return 0;
    }
    make_crc_table();

    /* every length at every alignment */
    for (len = 0; len <= 4096; len++)
        for (off = 0; off < 64; off++) {
            unsigned long c0 = len & 1 ? rnd() : 0, a0 = len & 1 ? (rnd() % 65521) << 16 | rnd() % 65521 : 1;
            same("crc32", crc32_z(c0, buf + off, len), ref_crc(c0, buf + off, len), len, off);
            same("adler32", adler32_z(a0, buf + off, len), ref_adler(a0, buf + off, len), len, off);
            if (off == 0) {
                same("adler32 ones", adler32_z(a0, ones, len), ref_adler(a0, ones, len), len, off);
                same("crc32 ones", crc32_z(c0, ones, len), ref_crc(c0, ones, len), len, off);
            }
            cases += 2;
        }

    /* long buffers, whole and in pieces, from all starting points */
    for (i = 0; i < 24; i++) {
        size_t n = i < 4 ? big - i : rnd() % big;
        unsigned char *p = i & 1 ? ones : buf;
        unsigned long c0 = i ? rnd() : 0, a0 = i ? (rnd() % 65521) << 16 | rnd() % 65521 : 1;
        unsigned long rc = ref_crc(c0, p + i % 7, n), ra = ref_adler(a0, p + i % 7, n);
        same("crc32 long", crc32_z(c0, p + i % 7, n), rc, n, i % 7);
        same("adler32 long", adler32_z(a0, p + i % 7, n), ra, n, i % 7);
        same("crc32 pieces", pieces(1, c0, p + i % 7, n), rc, n, i % 7);
        same("adler32 pieces", pieces(0, a0, p + i % 7, n), ra, n, i % 7);
        cases += 4;
    }

    /* combining */
    for (i = 0; i < 2000; i++) {
        size_t n1 = rnd() % (i < 1000 ? 5000 : big / 2), n2 = rnd() % (i < 1000 ? 5000 : big / 2);
        unsigned long c1 = crc32_z(0, buf, n1), c2 = crc32_z(0, buf + n1, n2);
        unsigned long a1 = adler32_z(1, buf, n1), a2 = adler32_z(1, buf + n1, n2);
        unsigned long whole_c = crc32_z(0, buf, n1 + n2), whole_a = adler32_z(1, buf, n1 + n2);
        same("crc32_combine", crc32_combine(c1, c2, (z_off_t)n2), whole_c, n1 + n2, n1);
        same("crc32_combine_op", crc32_combine_op(c1, c2, crc32_combine_gen((z_off_t)n2)), whole_c, n1 + n2, n1);
        same("adler32_combine", adler32_combine(a1, a2, (z_off_t)n2), whole_a, n1 + n2, n1);
        cases += 3;
        if (i >= 1000) i += 49;     /* fewer of the long ones */
    }
    printf("%ld cases, %ld differ\n", cases, bad);
    free(buf);
    free(ones);
    return bad != 0;
}
