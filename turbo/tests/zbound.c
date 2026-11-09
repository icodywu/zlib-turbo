/* zbound.c -- inflate() writes nothing past the output space it is given

   usage: zbound [seed [size [k0 [k1]]]]

   Makes data of periodic stretches, each with a new period string, so that runs of
   literals end in long matches, compresses it as raw deflate at level 9, and inflates it
   in three calls for every split: the first call stops after p bytes of output, the
   second gets exactly k bytes of output space, the third gets the rest. p runs over every
   output position and k from k0 to k1 (default 258 to 600), which puts the end of the
   second call's output space at every position after every code sequence, where inflate()
   must stop short. Thirty-two bytes after each call's output space hold a pattern that
   must survive the call, and the result must be the data. Prints "ok" and returns 0, or
   prints the first failures and returns 1. Under AddressSanitizer the same holds for the
   input: each call gets exactly the remaining input, in a buffer of that size. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "zlib.h"

#define GUARD 32

static unsigned long long rs;

static unsigned rnd(void) {
    rs ^= rs << 13;
    rs ^= rs >> 7;
    rs ^= rs << 17;
    return (unsigned)(rs >> 11);
}

/* One inflate() call with exactly the remaining input and room bytes of output space
   followed by GUARD bytes of pattern. Appends what it writes to dst at *got. Returns
   inflate()'s return code, or 99 if the pattern changed. */
static int call(z_stream *s, const unsigned char *z, size_t zn, size_t *ipos,
                unsigned char *dst, size_t room, size_t *got, int flush) {
    size_t nin = zn - *ipos, k;
    unsigned char *ib = malloc(nin ? nin : 1), *ob = malloc(room + GUARD);
    int ret;

    memcpy(ib, z + *ipos, nin);
    for (k = 0; k < GUARD; k++)
        ob[room + k] = (unsigned char)(0xa5 ^ k);
    s->next_in = ib;
    s->avail_in = (uInt)nin;
    s->next_out = ob;
    s->avail_out = (uInt)room;
    ret = inflate(s, flush);
    for (k = 0; k < GUARD; k++)
        if (ob[room + k] != (unsigned char)(0xa5 ^ k))
            ret = 99;
    *ipos += nin - s->avail_in;
    memcpy(dst + *got, ob, room - s->avail_out);
    *got += room - s->avail_out;
    free(ib);
    free(ob);
    return ret;
}

int main(int argc, char **argv) {
    unsigned long long seed = argc > 1 ? strtoull(argv[1], NULL, 10) : 1;
    size_t n = argc > 2 ? strtoul(argv[2], NULL, 10) : 3000;
    size_t k0 = argc > 3 ? strtoul(argv[3], NULL, 10) : 258;
    size_t k1 = argc > 4 ? strtoul(argv[4], NULL, 10) : 600;
    unsigned char *d = malloc(n + 1), *z, *dst = malloc(n + 2);
    size_t i = 0, p, k, zn, bad = 0, runs = 0;
    z_stream c, s;

    rs = seed * 0x9e3779b97f4a7c15ULL + 1;
    while (i < n) {
        size_t per = 16 + rnd() % 250, len = per + 258 + rnd() % 300, j;
        unsigned char q[266];

        for (j = 0; j < per; j++)
            q[j] = (unsigned char)rnd();
        for (j = 0; j < len && i < n; j++)
            d[i++] = q[j % per];
        if (rnd() % 2 && i + 2 <= n) {
            d[i++] = (unsigned char)rnd();
            d[i++] = (unsigned char)rnd();
        }
    }

    zn = compressBound((uLong)n) + 64;
    z = malloc(zn);
    memset(&c, 0, sizeof(c));
    if (deflateInit2(&c, 9, Z_DEFLATED, -15, 9, Z_DEFAULT_STRATEGY) != Z_OK)
        return 2;
    c.next_in = d;
    c.avail_in = (uInt)n;
    c.next_out = z;
    c.avail_out = (uInt)zn;
    if (deflate(&c, Z_FINISH) != Z_STREAM_END)
        return 2;
    zn = c.total_out;
    deflateEnd(&c);

    for (p = 0; p < n; p++)
        for (k = k0; k <= k1; k++) {
            size_t ipos = 0, got = 0;
            int ret;

            runs++;
            memset(&s, 0, sizeof(s));
            if (inflateInit2(&s, -15) != Z_OK)
                return 2;
            ret = call(&s, z, zn, &ipos, dst, p, &got, Z_NO_FLUSH);
            if (ret == Z_OK || ret == Z_BUF_ERROR)
                ret = call(&s, z, zn, &ipos, dst, k, &got, Z_NO_FLUSH);
            if (ret == Z_OK || ret == Z_BUF_ERROR)
                ret = call(&s, z, zn, &ipos, dst, n - got + 1, &got, Z_FINISH);
            inflateEnd(&s);
            if (ret != Z_STREAM_END || got != n || memcmp(dst, d, n)) {
                if (bad++ < 5)
                    printf("FAIL seed %llu p %lu k %lu: %s\n", seed, (unsigned long)p,
                           (unsigned long)k, ret == 99 ? "wrote past the output space" :
                           "wrong result");
            }
        }
    printf("seed %llu, %lu bytes, %lu splits: %s\n", seed, (unsigned long)n,
           (unsigned long)runs, bad ? "FAILED" : "ok");
    free(d);
    free(z);
    free(dst);
    return bad != 0;
}
