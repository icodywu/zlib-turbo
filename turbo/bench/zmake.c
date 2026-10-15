/* zmake: compress each file to file.z (zlib format) at the given level with the linked zlib's deflate.
   usage: zmake level file... */
#include <stdio.h>
#include <stdlib.h>
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
        uLongf cn = compressBound(n);
        unsigned char* c = malloc(cn);
        if (compress2(c, &cn, p, n, level) != Z_OK) return 1;
        char name[4096]; snprintf(name, sizeof name, "%s.z", argv[i]);
        f = fopen(name, "wb");
        fwrite(c, 1, cn, f); fclose(f);
        printf("%s %zu -> %lu\n", argv[i], n, (unsigned long)cn);
        free(p); free(c);
    }
    return 0;
}
