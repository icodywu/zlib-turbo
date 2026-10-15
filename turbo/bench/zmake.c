/* zmake: compress each file to file.z (zlib format, or gzip with ZGZIP set in the environment) at the given level
   with the linked zlib's deflate, as compress2() would (windowBits 15, memLevel 8, default strategy).
   usage: zmake level file... */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "zlib.h"

int main(int argc, char** argv)
{
    const int level = atoi(argv[1]);
    for (int i = 2; i < argc; i++) {
        FILE* f = fopen(argv[i], "rb");
        if (!f) { perror(argv[i]); return 1; }
        fseek(f, 0, SEEK_END); size_t n = (size_t)ftell(f); fseek(f, 0, SEEK_SET);
        unsigned char* p = malloc(n + 1);
        if (fread(p, 1, n, f) != n) return 1;
        fclose(f);
        uLongf cn = compressBound(n) + 18;
        unsigned char* c = malloc(cn);
        z_stream s; memset(&s, 0, sizeof s);
        if (deflateInit2(&s, level, Z_DEFLATED, getenv("ZGZIP") ? 31 : 15, 8, Z_DEFAULT_STRATEGY) != Z_OK) return 1;
        s.next_in = p; s.avail_in = (uInt)n; s.next_out = c; s.avail_out = (uInt)cn;
        if (deflate(&s, Z_FINISH) != Z_STREAM_END) return 1;
        cn = s.total_out;
        deflateEnd(&s);
        char name[4096]; snprintf(name, sizeof name, "%s.z", argv[i]);
        f = fopen(name, "wb");
        fwrite(c, 1, cn, f); fclose(f);
        printf("%s %zu -> %lu\n", argv[i], n, (unsigned long)cn);
        free(p); free(c);
    }
    return 0;
}
