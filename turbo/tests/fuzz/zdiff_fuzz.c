/* libFuzzer target: inflate of stock zlib (a shared library opened with dlopen, not instrumented) and of
   the patched zlib (linked in, with ASAN or UBSAN) must agree on every input: the same return code, the same
   number of bytes out, and the same bytes, with the same input and output pieces.
   Input: byte 0 = mode (bit 7: the rest is data to deflate first with the stock deflate, level = bits 0-3 % 10,
   strategy = bits 4-5; otherwise the rest is a stream), byte 1 = window bits choice, byte 2 = n input piece sizes,
   then the sizes, then 1 byte = n output piece sizes, then those, then the payload. STOCK_SO names the library. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "zlib.h"

typedef struct {
    int (*inflateInit2_)(z_streamp, int, const char *, int);
    int (*inflate)(z_streamp, int);
    int (*inflateEnd)(z_streamp);
    int (*deflateInit2_)(z_streamp, int, int, int, int, int, const char *, int);
    int (*deflate)(z_streamp, int);
    int (*deflateEnd)(z_streamp);
    uLong (*deflateBound)(z_streamp, uLong);
    const char *(*zlibVersion)(void);
} Api;

static Api stock, fast = { inflateInit2_, inflate, inflateEnd, deflateInit2_, deflate, deflateEnd, deflateBound,
                           zlibVersion };

static void load_stock(void)
{
    void *h = dlopen(STOCK_SO, RTLD_NOW | RTLD_LOCAL);
    if (!h) { fprintf(stderr, "dlopen: %s\n", dlerror()); exit(1); }
#define GET(f) *(void **)&stock.f = dlsym(h, #f); if (!stock.f) { fprintf(stderr, "no %s\n", #f); exit(1); }
    GET(inflateInit2_) GET(inflate) GET(inflateEnd) GET(deflateInit2_) GET(deflate) GET(deflateEnd)
    GET(deflateBound) GET(zlibVersion)
    fprintf(stderr, "stock %s, patched %s\n", stock.zlibVersion(), fast.zlibVersion());
}

/* decode in[0, n) in pieces from sizes[] and with output space from osizes[] (cycled), each input piece in a buffer
   of its own; returns the last return code and sets *got */
static int decode(const Api *a, const unsigned char *in, size_t n, int wbits, const unsigned char *sizes,
                  size_t nsizes, const unsigned char *osizes, size_t nosizes, unsigned char *out, size_t cap,
                  size_t *got)
{
    z_stream s;
    size_t pos = 0, total = 0, k = 0, ko = 0;
    unsigned char *piece = NULL;
    int r, stall = 0;

    memset(&s, 0, sizeof s);
    if (a->inflateInit2_(&s, wbits, ZLIB_VERSION, (int)sizeof(z_stream)) != Z_OK) return -99;
    for (;;) {
        unsigned char ob[1024];
        size_t want, made;
        unsigned before;
        if (s.avail_in == 0 && pos < n) {
            size_t m = 1 + (nsizes ? sizes[k++ % nsizes] : 255);
            if (m > n - pos) m = n - pos;
            free(piece);
            piece = malloc(m);
            memcpy(piece, in + pos, m);
            pos += m;
            s.next_in = piece;
            s.avail_in = (unsigned)m;
        }
        want = 1 + (nosizes ? (size_t)osizes[ko++ % nosizes] * 4 : 1023);
        if (want > sizeof ob) want = sizeof ob;
        s.next_out = ob;
        s.avail_out = (unsigned)want;
        before = s.avail_in;
        r = a->inflate(&s, Z_NO_FLUSH);
        made = want - s.avail_out;
        if (total + made > cap) { r = -98; break; }
        memcpy(out + total, ob, made);
        total += made;
        if (r != Z_OK && r != Z_BUF_ERROR) break;
        if (made == 0 && s.avail_in == before) { if (++stall > 2) break; } else stall = 0;
    }
    free(piece);
    a->inflateEnd(&s);
    *got = total;
    return r;
}

static int deflate_buf(const unsigned char *in, size_t n, int level, int wbits, int strategy, unsigned char *out,
                       size_t *cn)
{
    z_stream s;
    int r;
    memset(&s, 0, sizeof s);
    if (wbits > 0 && wbits < 9) wbits = 9;
    if (wbits < 0 && wbits > -9) wbits = -9;
    if (stock.deflateInit2_(&s, level, Z_DEFLATED, wbits, 8, strategy, ZLIB_VERSION, (int)sizeof(z_stream)) != Z_OK)
        return -1;
    s.next_in = (unsigned char *)in; s.avail_in = (unsigned)n;
    s.next_out = out; s.avail_out = (unsigned)stock.deflateBound(&s, n);
    r = stock.deflate(&s, Z_FINISH);
    *cn = s.total_out;
    stock.deflateEnd(&s);
    return r == Z_STREAM_END ? 0 : -1;
}

#define CAP (1u << 20)

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    static unsigned char *o1, *o2, *c;
    static const int wb[8] = { 15, -15, 31, 9, -9, 12, -12, 47 };
    size_t p = 3, ns, nos, n1, n2, cn;
    const unsigned char *sizes, *osizes, *payload;
    int mode, w, r1, r2;

    if (!o1) { load_stock(); o1 = malloc(CAP); o2 = malloc(CAP); c = malloc(CAP + 4096); }
    if (size < 4) return 0;
    mode = data[0];
    w = wb[data[1] & 7];
    ns = data[2] % 16;
    if (p + ns >= size) return 0;
    sizes = data + p; p += ns;
    nos = data[p++] % 16;
    if (p + nos > size) return 0;
    osizes = data + p; p += nos;
    payload = data + p;
    cn = size - p;
    if (mode & 0x80) {                      /* compress first: a valid stream */
        int lev = (mode & 15) % 10, strat = (mode >> 4) & 3;
        if (cn > 65536) cn = 65536;
        if (deflate_buf(payload, cn, lev, w == 47 ? 15 : w, strat, c, &cn) != 0) return 0;
        payload = c;
    }
    r1 = decode(&stock, payload, cn, w, sizes, ns, osizes, nos, o1, CAP, &n1);
    r2 = decode(&fast, payload, cn, w, sizes, ns, osizes, nos, o2, CAP, &n2);
    if (r1 != r2 || n1 != n2 || memcmp(o1, o2, n1)) {
        fprintf(stderr, "MISMATCH: stock %d %zu, patched %d %zu\n", r1, n1, r2, n2);
        abort();
    }
    return 0;
}
