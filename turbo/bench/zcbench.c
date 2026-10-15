/* zcbench: compression speed of a zlib-API library, each file whole with compress2 at a level (best of the runs in
   at least 1 s, at least 2 runs), plus a hash of all compressed bytes so that builds can be checked for identical
   output.   usage: zcbench level file...  (env ZC_ONCE=1: one run, for profilers) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "zlib.h"
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }
int main(int argc, char **argv) {
    int level = atoi(argv[1]), once = getenv("ZC_ONCE") != NULL;
    double tin = 0, tt = 0, tout = 0;
    unsigned long long h = 1469598103934665603ULL;
    for (int i = 2; i < argc; i++) {
        FILE *f = fopen(argv[i], "rb"); if (!f) { perror(argv[i]); return 1; }
        fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
        unsigned char *p = malloc(n + 1); if (fread(p, 1, n, f) != (size_t)n) return 1; fclose(f);
        uLong cap = compressBound(n); unsigned char *c = malloc(cap); uLongf cn = cap;
        double best = 1e30, total = 0; int reps = 0;
        do {
            cn = cap; double t0 = now();
            if (compress2(c, &cn, p, n, level) != Z_OK) { printf("%s: FAILED\n", argv[i]); return 2; }
            double t = now() - t0; if (t < best) best = t; total += t; reps++;
        } while (!once && (total < 1.0 || reps < 2));
        for (uLong j = 0; j < cn; j++) h = (h ^ c[j]) * 1099511628211ULL;
        tin += n; tout += cn; tt += best;
        free(p); free(c);
    }
    printf("TOTAL L%d ratio %.4f comp %.2f MB/s hash %016llx\n", level, tin / tout, tin / tt / 1e6, h);
    return 0;
}
