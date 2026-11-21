/* zback.c -- inflateBack() keeps the history in its window intact

   usage: zback [seed]

   inflateBack() decodes into its window, so once the window has wrapped, the bytes just
   past each match are history that a later match may copy. zlib's deflate never makes
   distances above 32506, so this writes its own raw deflate stream, one fixed-Huffman
   block: 40000 literals to fill and wrap the window, then pairs of matches, the first
   from 16 to 399 bytes back and the second from 32754 to 32768 bytes back, which copies
   the bytes just past the first. It decodes the stream with inflateBack() and with
   inflate(), in one call and in calls of one byte of input at a time, and both must give
   the data. Prints "ok" and returns 0, or prints the failures and returns 1. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "zlib.h"

static unsigned long long rs;

static unsigned rnd(unsigned n) {
    rs ^= rs << 13;
    rs ^= rs >> 7;
    rs ^= rs << 17;
    return (unsigned)((rs >> 11) % n);
}

/* bit writer for the stream */
static unsigned char *zb;
static size_t zn;
static unsigned long acc;
static int nacc;

static void put_bits(unsigned long v, int n) {
    acc |= v << nacc;
    nacc += n;
    while (nacc >= 8) {
        zb[zn++] = (unsigned char)acc;
        acc >>= 8;
        nacc -= 8;
    }
}

static void put_code(unsigned code, int n) {     /* Huffman codes go first bit first */
    unsigned long rev = 0;
    int i;
    for (i = 0; i < n; i++)
        rev |= (unsigned long)((code >> (n - 1 - i)) & 1) << i;
    put_bits(rev, n);
}

static void put_sym(unsigned v) {                /* fixed literal/length code */
    if (v < 144)
        put_code(0x30 + v, 8);
    else if (v < 256)
        put_code(0x190 + v - 144, 9);
    else if (v < 280)
        put_code(v - 256, 7);
    else
        put_code(0xc0 + v - 280, 8);
}

static const unsigned lbase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27,
    31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
static const int lext[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3,
    4, 4, 4, 4, 5, 5, 5, 5, 0};
static const unsigned dbase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129,
    193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385,
    24577};
static const int dext[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8,
    9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

static unsigned char *data;
static size_t dn;

static void literal(unsigned b) {
    put_sym(b);
    data[dn++] = (unsigned char)b;
}

static void match(unsigned len, unsigned dist) {
    int i = 28, j = 29;
    while (lbase[i] > len)
        i--;
    put_sym(257 + i);
    put_bits(len - lbase[i], lext[i]);
    while (dbase[j] > dist)
        j--;
    put_code(j, 5);
    put_bits(dist - dbase[j], dext[j]);
    while (len--) {
        data[dn] = data[dn - dist];
        dn++;
    }
}

/* inflateBack() callbacks: input in pieces of step bytes, output appended to got */
static const unsigned char *next;
static size_t left, step;
static unsigned char *got;
static size_t ngot;

static unsigned in_cb(void *d, z_const unsigned char **buf) {
    size_t n = left < step ? left : step;
    (void)d;
    *buf = (z_const unsigned char *)next;
    next += n;
    left -= n;
    return (unsigned)n;
}

static int out_cb(void *d, unsigned char *buf, unsigned len) {
    (void)d;
    if (ngot + len > dn)
        return 1;
    memcpy(got + ngot, buf, len);
    ngot += len;
    return 0;
}

static int check(const char *what, int ret) {
    size_t i = 0;
    while (i < ngot && i < dn && got[i] == data[i])
        i++;
    if (ret == Z_STREAM_END && ngot == dn && i == dn)
        return 0;
    printf("zback: %s, input in steps of %lu: return %d, %lu of %lu bytes, first "
           "difference at %lu\n", what, (unsigned long)step, ret, (unsigned long)ngot,
           (unsigned long)dn, (unsigned long)i);
    return 1;
}

int main(int argc, char **argv) {
    int pass, fail = 0;
    unsigned k;
    unsigned char window[32768];
    z_stream s;

    rs = 0x9e3779b97f4a7c15ULL ^ (unsigned long long)(argc > 1 ? atoi(argv[1]) : 1);
    zb = malloc(400000);
    data = malloc(2000000);
    got = malloc(2000000);
    if (zb == NULL || data == NULL || got == NULL)
        return 1;
    put_bits(1, 1);                             /* last block */
    put_bits(1, 2);                             /* fixed Huffman codes */
    for (k = 0; k < 40000; k++)
        literal(rnd(256));
    for (k = 0; k < 20000; k++) {
        match(3 + rnd(37), 16 + rnd(384));
        match(3 + rnd(37), 32754 + rnd(15));
        if (rnd(10) < 3)
            literal(rnd(256));
    }
    put_sym(256);
    if (nacc)
        put_bits(0, 8 - nacc);

    for (pass = 0; pass < 2; pass++) {
        step = pass ? 1 : zn;

        memset(&s, 0, sizeof(s));
        if (inflateBackInit(&s, 15, window) != Z_OK)
            return 1;
        next = zb;
        left = zn;
        ngot = 0;
        fail |= check("inflateBack()", inflateBack(&s, in_cb, NULL, out_cb, NULL));
        inflateBackEnd(&s);

        memset(&s, 0, sizeof(s));
        if (inflateInit2(&s, -15) != Z_OK)
            return 1;
        s.next_out = got;
        s.avail_out = (uInt)dn;
        {
            size_t pos = 0;
            int ret = Z_OK;
            while (ret == Z_OK && pos < zn) {
                size_t n = zn - pos < step ? zn - pos : step;
                s.next_in = zb + pos;
                s.avail_in = (uInt)n;
                ret = inflate(&s, Z_NO_FLUSH);
                pos += n - s.avail_in;
            }
            ngot = dn - s.avail_out;
            fail |= check("inflate()", ret);
        }
        inflateEnd(&s);
    }
    free(zb);
    free(data);
    free(got);
    if (!fail)
        printf("zback: ok (%lu bytes from %lu)\n", (unsigned long)dn, (unsigned long)zn);
    return fail;
}
