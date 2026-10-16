/* Copyright (C) 2026 Yingquan (Cody) Wu
 * For conditions of distribution and use, see copyright notice in zlib.h
 */

/* syncflush: interactive streams (SSH, websocket permessage-deflate, HTTP streaming) rely on inflate giving out every
   byte before a Z_SYNC_FLUSH point once the compressed bytes up to that point are in. Each input is cut into random
   messages, deflated with Z_SYNC_FLUSH after each; inflate is fed exactly each message's compressed bytes (in one
   piece) and must have produced exactly that message's bytes before the next is fed.
   usage: syncflush file...   prints per file the number of messages and of messages held back */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "zlib.h"

static unsigned long long rng = 88172645463325252ull;
static unsigned rnd(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return (unsigned)(rng >> 11); }

int main(int argc, char** argv)
{
    long held_total = 0, msgs_total = 0;
    for (int i = 1; i < argc; i++) {
        FILE* f = fopen(argv[i], "rb");
        if (!f) continue;
        fseek(f, 0, SEEK_END); size_t n = (size_t)ftell(f); fseek(f, 0, SEEK_SET);
        if (n > (4u << 20)) n = 4u << 20;
        unsigned char* p = malloc(n + 1);
        if (fread(p, 1, n, f) != n) return 1;
        fclose(f);
        for (int level = 1; level <= 9; level += 4) {
            z_stream d, s; memset(&d, 0, sizeof d); memset(&s, 0, sizeof s);
            deflateInit(&d, level); inflateInit(&s);
            unsigned char* c = malloc(compressBound(n) + 1024 * 1024);
            unsigned char* o = malloc(n + 64);
            size_t pos = 0, cpos = 0, opos = 0;
            long msgs = 0, held = 0;
            while (pos < n) {
                size_t k = 1 + rnd() % (rnd() % 2 ? 64 : 8192); if (k > n - pos) k = n - pos;
                d.next_in = p + pos; d.avail_in = (uInt)k; d.next_out = c + cpos; d.avail_out = (uInt)(compressBound(n) + 1024 * 1024 - cpos);
                deflate(&d, Z_SYNC_FLUSH);
                const size_t cn = (size_t)(d.next_out - (c + cpos));
                s.next_in = c + cpos; s.avail_in = (uInt)cn; s.next_out = o + opos; s.avail_out = (uInt)(n + 64 - opos);
                int r = inflate(&s, Z_SYNC_FLUSH);
                const size_t got = (size_t)(s.next_out - (o + opos));
                if (r != Z_OK && r != Z_BUF_ERROR) { printf("%s L%d: inflate error %d at message %ld\n", argv[i], level, r, msgs); break; }
                if (got != k || memcmp(o + opos, p + pos, got)) held++;
                opos += got; pos += k; cpos += cn; msgs++;
            }
            printf("%-14s level %d: %ld messages, %ld not fully delivered at their flush point\n", strrchr(argv[i], '/') ? strrchr(argv[i], '/') + 1 : argv[i], level, msgs, held);
            msgs_total += msgs; held_total += held;
            deflateEnd(&d); inflateEnd(&s); free(c); free(o);
        }
        free(p);
    }
    printf("%s: %ld messages, %ld held back\n", zlibVersion(), msgs_total, held_total);
    return 0;
}
