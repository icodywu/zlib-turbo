/* deflate.c -- compress data using the deflation algorithm
 * Copyright (C) 1995-2026 Jean-loup Gailly and Mark Adler
 * For conditions of distribution and use, see copyright notice in zlib.h
 */

/*
 *  ALGORITHM
 *
 *      The "deflation" process depends on being able to identify portions
 *      of the input text which are identical to earlier input (within a
 *      sliding window trailing behind the input currently being processed).
 *
 *      The most straightforward technique turns out to be the fastest for
 *      most input files: try all possible matches and select the longest.
 *      The key feature of this algorithm is that insertions into the string
 *      dictionary are very simple and thus fast, and deletions are avoided
 *      completely. Insertions are performed at each input character, whereas
 *      string matches are performed only when the previous match ends. So it
 *      is preferable to spend more time in matches to allow very fast string
 *      insertions and avoid deletions. The matching algorithm for small
 *      strings is inspired from that of Rabin & Karp. A brute force approach
 *      is used to find longer strings when a small match has been found.
 *      A similar algorithm is used in comic (by Jan-Mark Wams) and freeze
 *      (by Leonid Broukhis).
 *         A previous version of this file used a more sophisticated algorithm
 *      (by Fiala and Greene) which is guaranteed to run in linear amortized
 *      time, but has a larger average cost, uses more memory and is patented.
 *      However the F&G algorithm may be faster for some highly redundant
 *      files if the parameter max_chain_length (described below) is too large.
 *
 *  ACKNOWLEDGEMENTS
 *
 *      The idea of lazy evaluation of matches is due to Jan-Mark Wams, and
 *      I found it in 'freeze' written by Leonid Broukhis.
 *      Thanks to many people for bug reports and testing.
 *
 *  REFERENCES
 *
 *      Deutsch, L.P.,"DEFLATE Compressed Data Format Specification".
 *      Available at https://datatracker.ietf.org/doc/html/rfc1951
 *
 *      A description of the Rabin and Karp algorithm is given in the book
 *         "Algorithms" by R. Sedgewick, Addison-Wesley, p252.
 *
 *      Fiala,E.R., and Greene,D.H.
 *         Data Compression with Finite Windows, Comm.ACM, 32,4 (1989) 490-595
 *
 */

/* @(#) $Id$ */

#include "deflate.h"
#include "zturbo.h"

const char deflate_copyright[] =
   " deflate 1.3.2 Copyright 1995-2026 Jean-loup Gailly and Mark Adler ";
/*
  If you use the zlib library in a product, an acknowledgment is welcome
  in the documentation of your product. If for some reason you cannot
  include such an acknowledgment, I would appreciate that you keep this
  copyright string in the executable of your product.
 */

typedef enum {
    need_more,      /* block not completed, need more input or more output */
    block_done,     /* block flush performed */
    finish_started, /* finish started, need only more output at next deflate */
    finish_done     /* finish done, accept no more input or output */
} block_state;

typedef block_state (*compress_func)(deflate_state *s, int flush);
/* Compression function. Returns the block state after the call. */

local block_state deflate_stored(deflate_state *s, int flush);
local block_state deflate_fast(deflate_state *s, int flush);
#ifndef FASTEST
local block_state deflate_slow(deflate_state *s, int flush);
#endif
#ifdef Z_TURBO_THREADS
local void mt_reset(deflate_state *s);
local void mt_free(z_streamp strm, deflate_state *s);
local void mt_catch_up(deflate_state *s, unsigned end);
local void mt_restart(deflate_state *s, Z_U8 at, int partial);
#endif
local block_state deflate_rle(deflate_state *s, int flush);
local block_state deflate_huff(deflate_state *s, int flush);

/* ===========================================================================
 * Local data
 */

#define NIL 0
/* Tail of hash chains */

#ifndef TOO_FAR
#  define TOO_FAR 4096
#endif
/* Matches of length 3 are discarded if their distance exceeds TOO_FAR */

/* Values for max_lazy_match, good_match and max_chain_length, depending on
 * the desired pack level (0..9). The values given below have been tuned to
 * exclude worst case performance for pathological files. Better values may be
 * found for specific files.
 */
typedef struct config_s {
   ush good_length; /* reduce lazy search above this match length */
   ush max_lazy;    /* do not perform lazy search above this match length */
   ush nice_length; /* quit search above this match length */
   ush max_chain;
   compress_func func;
} config;

#ifdef FASTEST
local const config configuration_table[2] = {
/*      good lazy nice chain */
/* 0 */ {0,    0,  0,    0, deflate_stored},  /* store only */
/* 1 */ {4,    4,  8,    4, deflate_fast}}; /* max speed, no lazy matches */
#else
local const config configuration_table[10] = {
/*      good lazy nice chain */
/* 0 */ {0,    0,  0,    0, deflate_stored},  /* store only */
/* 1 */ {4,    4,  8,    4, deflate_fast}, /* max speed, no lazy matches */
/* 2 */ {4,    5, 16,    8, deflate_fast},
/* 3 */ {4,    6, 32,   32, deflate_fast},

/* 4 */ {4,    4, 16,   16, deflate_slow},  /* lazy matches */
/* 5 */ {8,   16, 32,   32, deflate_slow},
/* 6 */ {8,   16, 128, 128, deflate_slow},
/* 7 */ {8,   32, 128, 256, deflate_slow},
/* 8 */ {32, 128, 258, 1024, deflate_slow},
/* 9 */ {32, 258, 258, 4096, deflate_slow}}; /* max compression */
#endif

/* Note: the deflate() code requires max_lazy >= MIN_MATCH and max_chain >= 4
 * For deflate_fast() (levels <= 3) good is ignored and lazy has a different
 * meaning.
 */

/* rank Z_BLOCK between Z_NO_FLUSH and Z_PARTIAL_FLUSH */
#define RANK(f) (((f) * 2) - ((f) > 4 ? 9 : 0))

/* ===========================================================================
 * Update a hash value with the given input byte
 * IN  assertion: all calls to UPDATE_HASH are made with consecutive input
 *    characters, so that a running hash key can be computed from the previous
 *    key instead of complete recalculation each time.
 */
#define UPDATE_HASH(s,h,c) (h = (((h) << s->hash_shift) ^ (c)) & s->hash_mask)


/* ===========================================================================
 * Insert string str in the dictionary and set match_head to the previous head
 * of the hash chain (the most recent string with same hash key). Return
 * the previous length of the hash chain.
 * If this file is compiled with -DFASTEST, the compression level is forced
 * to 1, and no hash chains are maintained.
 * IN  assertion: all calls to INSERT_STRING are made with consecutive input
 *    characters and the first MIN_MATCH bytes of str are valid (except for
 *    the last MIN_MATCH-1 bytes of the input file).
 */
#ifdef FASTEST
#define INSERT_STRING(s, str, match_head) \
   (UPDATE_HASH(s, s->ins_h, s->window[(str) + (MIN_MATCH-1)]), \
    match_head = s->head[s->ins_h], \
    s->head[s->ins_h] = (Pos)(str))
#else
#define INSERT_STRING(s, str, match_head) \
   (UPDATE_HASH(s, s->ins_h, s->window[(str) + (MIN_MATCH-1)]), \
    match_head = s->prev[(str) & s->w_mask] = s->head[s->ins_h], \
    s->head[s->ins_h] = (Pos)(str))
#endif

/* ===========================================================================
 * Initialize the hash table (avoiding 64K overflow for 16 bit systems).
 * prev[] will be initialized on the fly.
 */
#define CLEAR_HASH(s) \
    do { \
        s->head[s->hash_size - 1] = NIL; \
        zmemzero(s->head, (unsigned)(s->hash_size - 1)*sizeof(*s->head)); \
        s->slid = 0; \
    } while (0)

/* ===========================================================================
 * Slide the hash table when sliding the window down (could be avoided with 32
 * bit values at the expense of memory usage). We slide even when level == 0 to
 * keep the hash table consistent if we switch back to level > 0 later.
 *
 * slide_table() subtracts wsize from the n entries at p, or sets them to NIL
 * if less than wsize. n is a multiple of eight, since hash_size and w_size are
 * powers of two no less than 256. The eight independent updates per step let
 * compilers use vector instructions (a saturating 16-bit subtraction). In
 * prev[], an entry not on any hash chain is garbage, but its value will never
 * be used.
 */
#if defined(__has_feature)
#  if __has_feature(memory_sanitizer)
     __attribute__((no_sanitize("memory")))
#  endif
#endif
local void slide_table(Posf *p, unsigned n, unsigned wsize) {
    Pos w = (Pos)wsize;
    int i;

    do {
        for (i = 0; i < 8; i++) {
            Pos m = p[i];
            p[i] = (Pos)(m >= w ? m - w : NIL);
        }
        p += 8;
        n -= 8;
    } while (n);
}

local void slide_hash(deflate_state *s) {
    slide_table(s->head, s->hash_size, s->w_size);
#ifndef FASTEST
    slide_table(s->prev, s->w_size, s->w_size);
#endif
    s->slid = 1;
}

/* ===========================================================================
 * Read a new buffer from the current input stream, update the adler32
 * and total number of bytes read.  All deflate() input goes through
 * this function so some applications may wish to modify it to avoid
 * allocating a large strm->next_in buffer and copying from it.
 * (See also flush_pending()).
 */
local unsigned read_buf(z_streamp strm, Bytef *buf, unsigned size) {
    unsigned len = strm->avail_in;

    if (len > size) len = size;
    if (len == 0) return 0;

    strm->avail_in  -= len;

    zmemcpy(buf, strm->next_in, len);
    if (strm->state->wrap == 1) {
        strm->adler = adler32(strm->adler, buf, len);
    }
#ifdef GZIP
    else if (strm->state->wrap == 2) {
        strm->adler = crc32(strm->adler, buf, len);
    }
#endif
    strm->next_in  += len;
    strm->total_in += len;

    return len;
}

/* ===========================================================================
 * Fill the window when the lookahead becomes insufficient.
 * Updates strstart and lookahead.
 *
 * IN assertion: lookahead < MIN_LOOKAHEAD
 * OUT assertions: strstart <= window_size-MIN_LOOKAHEAD
 *    At least one byte has been read, or avail_in == 0; reads are
 *    performed for at least two bytes (required for the zip translate_eol
 *    option -- not supported here).
 */
local void fill_window(deflate_state *s) {
    unsigned n;
    unsigned more;    /* Amount of free space at the end of the window. */
    uInt wsize = s->w_size;

    Assert(s->lookahead < MIN_LOOKAHEAD, "already enough lookahead");

    do {
        more = (unsigned)(s->window_size -(ulg)s->lookahead -(ulg)s->strstart);

        /* Deal with !@#$% 64K limit: */
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4127)
#endif
        if (sizeof(int) <= 2) {
#ifdef _MSC_VER
#pragma warning(pop)
#endif
            if (more == 0 && s->strstart == 0 && s->lookahead == 0) {
                more = wsize;

            } else if (more == (unsigned)(-1)) {
                /* Very unlikely, but possible on 16 bit machine if
                 * strstart == 0 && lookahead == 1 (input done a byte at time)
                 */
                more--;
            }
        }

        /* If the window is almost full and there is insufficient lookahead,
         * move the upper half to the lower one to make room in the upper half.
         */
        if (s->strstart >= wsize + MAX_DIST(s)) {

            zmemcpy(s->window, s->window + wsize, (unsigned)wsize - more);
            s->match_start -= wsize;
            s->strstart    -= wsize; /* we now have strstart >= MAX_DIST */
            s->block_start -= (long) wsize;
            if (s->insert > s->strstart)
                s->insert = s->strstart;
#ifdef Z_TURBO_THREADS
            if (s->mt_lag)
                s->mt_slides++;     /* done when the parse catches up */
            else
#endif
            {
                slide_hash(s);
            }
#ifdef Z_TURBO_THREADS
            s->mt_abs += wsize;
#endif
            more += wsize;
        }
        if (s->strm->avail_in == 0) break;

        /* If there was no sliding:
         *    strstart <= WSIZE+MAX_DIST-1 && lookahead <= MIN_LOOKAHEAD - 1 &&
         *    more == window_size - lookahead - strstart
         * => more >= window_size - (MIN_LOOKAHEAD-1 + WSIZE + MAX_DIST-1)
         * => more >= window_size - 2*WSIZE + 2
         * In the BIG_MEM or MMAP case (not yet supported),
         *   window_size == input_size + MIN_LOOKAHEAD  &&
         *   strstart + s->lookahead <= input_size => more >= MIN_LOOKAHEAD.
         * Otherwise, window_size == 2*WSIZE so more >= 2.
         * If there was sliding, more >= WSIZE. So in all cases, more >= 2.
         */
        Assert(more >= 2, "more < 2");

        n = read_buf(s->strm, s->window + s->strstart + s->lookahead, more);
        s->lookahead += n;

        /* Initialize the hash value now that we have some input: */
#ifdef Z_TURBO_THREADS
        if (s->insert && s->mt_lag)
            mt_catch_up(s, s->strstart - s->insert);
#endif
        if (s->lookahead + s->insert >= MIN_MATCH) {
            uInt str = s->strstart - s->insert;
            s->ins_h = s->window[str];
            UPDATE_HASH(s, s->ins_h, s->window[str + 1]);
#if MIN_MATCH != 3
            Call UPDATE_HASH() MIN_MATCH-3 more times
#endif
            while (s->insert) {
                UPDATE_HASH(s, s->ins_h, s->window[str + MIN_MATCH-1]);
#ifndef FASTEST
                s->prev[str & s->w_mask] = s->head[s->ins_h];
#endif
                s->head[s->ins_h] = (Pos)str;
                str++;
                s->insert--;
                if (s->lookahead + s->insert < MIN_MATCH)
                    break;
            }
        }
        /* If the whole input has less than MIN_MATCH bytes, ins_h is garbage,
         * but this is not important since only literal bytes will be emitted.
         */

    } while (s->lookahead < MIN_LOOKAHEAD && s->strm->avail_in != 0);

    /* If the WIN_INIT bytes after the end of the current data have never been
     * written, then zero those bytes in order to avoid memory check reports of
     * the use of uninitialized (or uninitialised as Julian writes) bytes by
     * the longest match routines.  Update the high water mark for the next
     * time through here.  WIN_INIT is set to MAX_MATCH since the longest match
     * routines allow scanning to strstart + MAX_MATCH, ignoring lookahead.
     */
    if (s->high_water < s->window_size) {
        ulg curr = s->strstart + (ulg)(s->lookahead);
        ulg init;

        if (s->high_water < curr) {
            /* Previous high water mark below current data -- zero WIN_INIT
             * bytes or up to end of window, whichever is less.
             */
            init = s->window_size - curr;
            if (init > WIN_INIT)
                init = WIN_INIT;
            zmemzero(s->window + curr, (unsigned)init);
            s->high_water = curr + init;
        }
        else if (s->high_water < (ulg)curr + WIN_INIT) {
            /* High water mark at or above current data, but below current data
             * plus WIN_INIT -- zero out to current data plus WIN_INIT, or up
             * to end of window, whichever is less.
             */
            init = (ulg)curr + WIN_INIT - s->high_water;
            if (init > s->window_size - s->high_water)
                init = s->window_size - s->high_water;
            zmemzero(s->window + s->high_water, (unsigned)init);
            s->high_water += init;
        }
    }

    Assert((ulg)s->strstart <= s->window_size - MIN_LOOKAHEAD,
           "not enough room for search");
}

/* ========================================================================= */
int ZEXPORT deflateInit_(z_streamp strm, int level, const char *version,
                         int stream_size) {
    return deflateInit2_(strm, level, Z_DEFLATED, MAX_WBITS, DEF_MEM_LEVEL,
                         Z_DEFAULT_STRATEGY, version, stream_size);
    /* To do: ignore strm->next_in if we use it as window */
}

/* ========================================================================= */
int ZEXPORT deflateInit2_(z_streamp strm, int level, int method,
                          int windowBits, int memLevel, int strategy,
                          const char *version, int stream_size) {
    deflate_state *s;
    int wrap = 1;
    static const char my_version[] = ZLIB_VERSION;

    if (version == Z_NULL || version[0] != my_version[0] ||
        stream_size != sizeof(z_stream)) {
        return Z_VERSION_ERROR;
    }
    if (strm == Z_NULL) return Z_STREAM_ERROR;

    strm->msg = Z_NULL;
    if (strm->zalloc == (alloc_func)0) {
#ifdef Z_SOLO
        return Z_STREAM_ERROR;
#else
        strm->zalloc = zcalloc;
        strm->opaque = (voidpf)0;
#endif
    }
    if (strm->zfree == (free_func)0)
#ifdef Z_SOLO
        return Z_STREAM_ERROR;
#else
        strm->zfree = zcfree;
#endif

#ifdef FASTEST
    if (level != 0) level = 1;
#else
    if (level == Z_DEFAULT_COMPRESSION) level = 6;
#endif

    if (windowBits < 0) { /* suppress zlib wrapper */
        wrap = 0;
        if (windowBits < -15)
            return Z_STREAM_ERROR;
        windowBits = -windowBits;
    }
#ifdef GZIP
    else if (windowBits > 15) {
        wrap = 2;       /* write gzip wrapper instead */
        windowBits -= 16;
    }
#endif
    if (memLevel < 1 || memLevel > MAX_MEM_LEVEL || method != Z_DEFLATED ||
        windowBits < 8 || windowBits > 15 || level < 0 || level > 9 ||
        strategy < 0 || strategy > Z_FIXED || (windowBits == 8 && wrap != 1)) {
        return Z_STREAM_ERROR;
    }
    if (windowBits == 8) windowBits = 9;  /* until 256-byte window bug fixed */
    s = (deflate_state *) ZALLOC(strm, 1, sizeof(deflate_state));
    if (s == Z_NULL) return Z_MEM_ERROR;
    zmemzero(s, sizeof(deflate_state));
    strm->state = (struct internal_state FAR *)s;
    s->strm = strm;
    s->status = INIT_STATE;     /* to pass state test in deflateReset() */

    s->wrap = wrap;
    s->gzhead = Z_NULL;
    s->w_bits = (uInt)windowBits;
    s->w_size = 1 << s->w_bits;
    s->w_mask = s->w_size - 1;

    s->hash_bits = (uInt)memLevel + 7;
    s->hash_size = 1 << s->hash_bits;
    s->hash_mask = s->hash_size - 1;
    s->hash_shift =  ((s->hash_bits + MIN_MATCH-1) / MIN_MATCH);

    s->window = (Bytef *) ZALLOC(strm, s->w_size, 2*sizeof(Byte));
    s->prev   = (Posf *)  ZALLOC(strm, s->w_size, sizeof(Pos));
    s->head   = (Posf *)  ZALLOC(strm, s->hash_size, sizeof(Pos));

    s->high_water = 0;      /* nothing written to s->window yet */

    s->lit_bufsize = 1 << (memLevel + 6); /* 16K elements by default */

    /* We overlay pending_buf and sym_buf. This works since the average size
     * for length/distance pairs over any compressed block is assured to be 31
     * bits or less.
     *
     * Analysis: The longest fixed codes are a length code of 8 bits plus 5
     * extra bits, for lengths 131 to 257. The longest fixed distance codes are
     * 5 bits plus 13 extra bits, for distances 16385 to 32768. The longest
     * possible fixed-codes length/distance pair is then 31 bits total.
     *
     * sym_buf starts one-fourth of the way into pending_buf. So there are
     * three bytes in sym_buf for every four bytes in pending_buf. Each symbol
     * in sym_buf is three bytes -- two for the distance and one for the
     * literal/length. As each symbol is consumed, the pointer to the next
     * sym_buf value to read moves forward three bytes. From that symbol, up to
     * 31 bits are written to pending_buf. The closest the written pending_buf
     * bits gets to the next sym_buf symbol to read is just before the last
     * code is written. At that time, 31*(n - 2) bits have been written, just
     * after 24*(n - 2) bits have been consumed from sym_buf. sym_buf starts at
     * 8*n bits into pending_buf. (Note that the symbol buffer fills when n - 1
     * symbols are written.) The closest the writing gets to what is unread is
     * then n + 14 bits. Here n is lit_bufsize, which is 16384 by default, and
     * can range from 128 to 32768.
     *
     * Therefore, at a minimum, there are 142 bits of space between what is
     * written and what is read in the overlain buffers, so the symbols cannot
     * be overwritten by the compressed data. That space is actually 139 bits,
     * due to the three-bit fixed-code block header.
     *
     * That covers the case where either Z_FIXED is specified, forcing fixed
     * codes, or when the use of fixed codes is chosen, because that choice
     * results in a smaller compressed block than dynamic codes. That latter
     * condition then assures that the above analysis also covers all dynamic
     * blocks. A dynamic-code block will only be chosen to be emitted if it has
     * fewer bits than a fixed-code block would for the same set of symbols.
     * Therefore its average symbol length is assured to be less than 31. So
     * the compressed data for a dynamic block also cannot overwrite the
     * symbols from which it is being constructed.
     */

    s->pending_buf = (uchf *) ZALLOC(strm, s->lit_bufsize, LIT_BUFS);
    s->pending_buf_size = (ulg)s->lit_bufsize * 4;

    if (s->window == Z_NULL || s->prev == Z_NULL || s->head == Z_NULL ||
        s->pending_buf == Z_NULL) {
        s->status = FINISH_STATE;
        strm->msg = ERR_MSG(Z_MEM_ERROR);
        deflateEnd (strm);
        return Z_MEM_ERROR;
    }
#ifdef LIT_MEM
    s->d_buf = (ushf *)(s->pending_buf + (s->lit_bufsize << 1));
    s->l_buf = s->pending_buf + (s->lit_bufsize << 2);
    s->sym_end = s->lit_bufsize - 1;
#else
    s->sym_buf = s->pending_buf + s->lit_bufsize;
    s->sym_end = (s->lit_bufsize - 1) * 3;
#endif
    /* We avoid equality with lit_bufsize*3 because of wraparound at 64K
     * on 16 bit machines and because stored blocks are restricted to
     * 64K-1 bytes.
     */

    s->level = level;
    s->strategy = strategy;
    s->method = (Byte)method;

    return deflateReset(strm);
}

/* =========================================================================
 * Check for a valid deflate stream state. Return 0 if ok, 1 if not.
 */
local int deflateStateCheck(z_streamp strm) {
    deflate_state *s;
    if (strm == Z_NULL ||
        strm->zalloc == (alloc_func)0 || strm->zfree == (free_func)0)
        return 1;
    s = strm->state;
    if (s == Z_NULL || s->strm != strm || (s->status != INIT_STATE &&
#ifdef GZIP
                                           s->status != GZIP_STATE &&
#endif
                                           s->status != EXTRA_STATE &&
                                           s->status != NAME_STATE &&
                                           s->status != COMMENT_STATE &&
                                           s->status != HCRC_STATE &&
                                           s->status != BUSY_STATE &&
                                           s->status != FINISH_STATE))
        return 1;
    return 0;
}

/* ========================================================================= */
int ZEXPORT deflateSetDictionary(z_streamp strm, const Bytef *dictionary,
                                 uInt  dictLength) {
    deflate_state *s;
    uInt str, n;
    int wrap;
    unsigned avail;
    z_const unsigned char *next;

    if (deflateStateCheck(strm) || dictionary == Z_NULL)
        return Z_STREAM_ERROR;
    s = strm->state;
    wrap = s->wrap;
    if (wrap == 2 || (wrap == 1 && s->status != INIT_STATE) || s->lookahead)
        return Z_STREAM_ERROR;

    /* when using zlib wrappers, compute Adler-32 for provided dictionary */
    if (wrap == 1)
        strm->adler = adler32(strm->adler, dictionary, dictLength);
    s->wrap = 0;                    /* avoid computing Adler-32 in read_buf */

    /* if dictionary would fill window, just replace the history */
    if (dictLength >= s->w_size) {
        if (wrap == 0) {            /* already empty otherwise */
#ifdef Z_TURBO_THREADS
            Z_U8 at = s->mt_abs + s->strstart;
#endif
            CLEAR_HASH(s);
            s->strstart = 0;
            s->block_start = 0L;
            s->insert = 0;
#ifdef Z_TURBO_THREADS
            mt_restart(s, at, 0);   /* the window starts again */
#endif
        }
        dictionary += dictLength - s->w_size;  /* use the tail */
        dictLength = s->w_size;
    }

    /* insert dictionary into window and hash */
    avail = strm->avail_in;
    next = strm->next_in;
    strm->avail_in = dictLength;
    strm->next_in = (z_const Bytef *)dictionary;
    fill_window(s);
    while (s->lookahead >= MIN_MATCH) {
        str = s->strstart;
        n = s->lookahead - (MIN_MATCH-1);
        do {
            UPDATE_HASH(s, s->ins_h, s->window[str + MIN_MATCH-1]);
#ifndef FASTEST
            s->prev[str & s->w_mask] = s->head[s->ins_h];
#endif
            s->head[s->ins_h] = (Pos)str;
            str++;
        } while (--n);
        s->strstart = str;
        s->lookahead = MIN_MATCH-1;
        fill_window(s);
    }
    s->strstart += s->lookahead;
    s->block_start = (long)s->strstart;
    s->insert = s->lookahead;
    s->lookahead = 0;
    s->match_length = s->prev_length = MIN_MATCH-1;
    s->match_available = 0;
    strm->next_in = next;
    strm->avail_in = avail;
    s->wrap = wrap;
#ifdef Z_TURBO_THREADS
    mt_reset(s);
#endif
    return Z_OK;
}

/* ========================================================================= */
int ZEXPORT deflateGetDictionary(z_streamp strm, Bytef *dictionary,
                                 uInt *dictLength) {
    deflate_state *s;
    uInt len;

    if (deflateStateCheck(strm))
        return Z_STREAM_ERROR;
    s = strm->state;
    len = s->strstart + s->lookahead;
    if (len > s->w_size)
        len = s->w_size;
    if (dictionary != Z_NULL && len)
        zmemcpy(dictionary, s->window + s->strstart + s->lookahead - len, len);
    if (dictLength != Z_NULL)
        *dictLength = len;
    return Z_OK;
}

/* ===========================================================================
 * Prepared dictionaries (zturbo.h). A prepared dictionary holds the state that
 * deflateSetDictionary() leaves on a stream just initialized or reset: the hash
 * table, the hash chains of the dictionary's strings, ins_h, and the number of
 * strings not yet inserted. Copying that state gives what indexing would give.
 */
struct z_dict_s {
    free_func zfree;        /* to free this */
    voidpf opaque;
    uInt w_bits;            /* windowBits and hash_bits of the stream */
    uInt hash_bits;
    uLong adler;            /* Adler-32 of the whole dictionary */
    uInt len;               /* length of the whole dictionary */
    uInt tail;              /* bytes in the window: the last w_size at most */
    uInt insert;            /* strings not yet inserted at the end */
    uInt ins_h;             /* hash after the last insertion */
    Bytef *bytes;           /* the whole dictionary */
    Posf *head;             /* the hash table, 1 << hash_bits entries */
    Posf *prev;             /* the chains, tail - insert entries */
};

/* Whether s is as deflateInit2() or deflateReset() left it, with nothing in
   its window or hash table. */
local int fresh_state(deflate_state *s) {
    return s->status == INIT_STATE && s->strstart == 0 && s->lookahead == 0 &&
           s->insert == 0 && s->block_start == 0;
}

/* ========================================================================= */
int ZEXPORT deflatePrepareDictionary(z_streamp strm, const Bytef *dictionary,
                                     uInt dictLength, z_dict **dict) {
    deflate_state *s;
    z_dict *d;
    ulg nhead, nprev, size;
    int ret;

    if (dict == Z_NULL)
        return Z_STREAM_ERROR;
    *dict = Z_NULL;
    if (deflateStateCheck(strm) || dictionary == Z_NULL)
        return Z_STREAM_ERROR;
    s = strm->state;
    if (!fresh_state(s))
        return Z_STREAM_ERROR;
    ret = deflateSetDictionary(strm, dictionary, dictLength);
    if (ret != Z_OK)
        return ret;

    /* one allocation: the header, the two tables, then the bytes */
    nhead = s->hash_size;
    nprev = s->strstart - s->insert;
    size = sizeof(z_dict) + (nhead + nprev) * sizeof(Pos) + dictLength;
    if (size < dictLength || size != (uInt)size)
        return Z_MEM_ERROR;
    d = (z_dict *)ZALLOC(strm, 1, (uInt)size);
    if (d == Z_NULL)
        return Z_MEM_ERROR;
    d->zfree = strm->zfree;
    d->opaque = strm->opaque;
    d->w_bits = s->w_bits;
    d->hash_bits = s->hash_bits;
    d->adler = adler32(1L, dictionary, dictLength);
    d->len = dictLength;
    d->tail = s->strstart;
    d->insert = s->insert;
    d->ins_h = s->ins_h;
    d->head = (Posf *)(d + 1);
    d->prev = d->head + nhead;
    d->bytes = (Bytef *)(d->prev + nprev);
    zmemcpy(d->head, s->head, (unsigned)(nhead * sizeof(Pos)));
#ifndef FASTEST
    zmemcpy(d->prev, s->prev, (unsigned)(nprev * sizeof(Pos)));
#endif
    zmemcpy(d->bytes, dictionary, dictLength);
    *dict = d;
    return Z_OK;
}

/* ========================================================================= */
int ZEXPORT deflateUseDictionary(z_streamp strm, const z_dict *dict) {
    deflate_state *s;
    unsigned avail;
    z_const unsigned char *next;

    if (deflateStateCheck(strm) || dict == Z_NULL)
        return Z_STREAM_ERROR;
    s = strm->state;
    if (s->wrap == 2 || !fresh_state(s) || dict->w_bits != s->w_bits ||
        dict->hash_bits != s->hash_bits)
        return deflateSetDictionary(strm, dict->bytes, dict->len);

    /* what deflateSetDictionary() does, with the indexing copied */
    if (s->wrap == 1)
        strm->adler = dict->adler;
    zmemcpy(s->window, dict->bytes + (dict->len - dict->tail), dict->tail);
    zmemcpy(s->head, dict->head, (unsigned)(s->hash_size * sizeof(Pos)));
#ifndef FASTEST
    zmemcpy(s->prev, dict->prev,
            (unsigned)((dict->tail - dict->insert) * sizeof(Pos)));
#endif
    s->ins_h = dict->ins_h;
    s->strstart = dict->tail;
    avail = strm->avail_in;             /* fill_window() with no input: */
    next = strm->next_in;               /* only the zeroing past the end */
    strm->avail_in = 0;
    fill_window(s);
    strm->next_in = next;
    strm->avail_in = avail;
    s->block_start = (long)s->strstart;
    s->insert = dict->insert;
    s->match_length = s->prev_length = MIN_MATCH-1;
    s->match_available = 0;
    strm->total_in += dict->tail;       /* as read_buf() counts it */
#ifdef Z_TURBO_THREADS
    mt_reset(s);
#endif
    return Z_OK;
}

/* ========================================================================= */
void ZEXPORT deflateFreeDictionary(z_dict *dict) {
    if (dict != Z_NULL)
        dict->zfree(dict->opaque, (voidpf)dict);
}

/* ========================================================================= */
int ZEXPORT deflateResetKeep(z_streamp strm) {
    deflate_state *s;

    if (deflateStateCheck(strm)) {
        return Z_STREAM_ERROR;
    }

    strm->total_in = strm->total_out = 0;
    strm->msg = Z_NULL; /* use zfree if we ever allocate msg dynamically */
    strm->data_type = Z_UNKNOWN;

    s = (deflate_state *)strm->state;
    s->pending = 0;
    s->pending_out = s->pending_buf;

    if (s->wrap < 0) {
        s->wrap = -s->wrap; /* was made negative by deflate(..., Z_FINISH); */
    }
    s->status =
#ifdef GZIP
        s->wrap == 2 ? GZIP_STATE :
#endif
        INIT_STATE;
    strm->adler =
#ifdef GZIP
        s->wrap == 2 ? crc32(0L, Z_NULL, 0) :
#endif
        adler32(0L, Z_NULL, 0);
    s->last_flush = -2;

    _tr_init(s);

    return Z_OK;
}

/* ===========================================================================
 * Initialize the "longest match" routines for a new zlib stream
 */
local void lm_init(deflate_state *s) {
    s->window_size = (ulg)2L*s->w_size;

    CLEAR_HASH(s);

    /* Set the default configuration parameters:
     */
    s->max_lazy_match   = configuration_table[s->level].max_lazy;
    s->good_match       = configuration_table[s->level].good_length;
    s->nice_match       = configuration_table[s->level].nice_length;
    s->max_chain_length = configuration_table[s->level].max_chain;

    s->strstart = 0;
    s->block_start = 0L;
    s->lookahead = 0;
    s->insert = 0;
    s->match_length = s->prev_length = MIN_MATCH-1;
    s->match_available = 0;
    s->ins_h = 0;
#ifdef Z_TURBO_THREADS
    s->mt_abs = 0;
    s->mt_exact = 1;
    s->mt_lag = 0;
    s->mt_slides = 0;
    s->mt_floor = 1;            /* window[0] is never matched */
    s->mt_from = 0;
    mt_reset(s);
#endif
}

/* ========================================================================= */
int ZEXPORT deflateReset(z_streamp strm) {
    int ret;

    ret = deflateResetKeep(strm);
    if (ret == Z_OK)
        lm_init(strm->state);
    return ret;
}

/* ========================================================================= */
int ZEXPORT deflateSetHeader(z_streamp strm, gz_headerp head) {
    if (deflateStateCheck(strm) || strm->state->wrap != 2)
        return Z_STREAM_ERROR;
    strm->state->gzhead = head;
    return Z_OK;
}

/* ========================================================================= */
int ZEXPORT deflatePending(z_streamp strm, unsigned *pending, int *bits) {
    if (deflateStateCheck(strm)) return Z_STREAM_ERROR;
    if (bits != Z_NULL)
        *bits = strm->state->bi_valid;
    if (pending != Z_NULL) {
        *pending = (unsigned)strm->state->pending;
        if (*pending != strm->state->pending) {
            *pending = (unsigned)-1;
            return Z_BUF_ERROR;
        }
    }
    return Z_OK;
}

/* ========================================================================= */
int ZEXPORT deflateUsed(z_streamp strm, int *bits) {
    if (deflateStateCheck(strm)) return Z_STREAM_ERROR;
    if (bits != Z_NULL)
        *bits = strm->state->bi_used;
    return Z_OK;
}

/* ========================================================================= */
int ZEXPORT deflatePrime(z_streamp strm, int bits, int value) {
    deflate_state *s;
    int put;

    if (deflateStateCheck(strm)) return Z_STREAM_ERROR;
    s = strm->state;
    /* zlib-turbo: where zlib's symbol buffer starts, also when threads use
       their own */
#ifdef LIT_MEM
    if (bits < 0 || bits > 16 ||
        s->pending_buf + (s->lit_bufsize << 1) <
            s->pending_out + ((Buf_size + 7) >> 3))
        return Z_BUF_ERROR;
#else
    if (bits < 0 || bits > 16 ||
        s->pending_buf + s->lit_bufsize <
            s->pending_out + ((Buf_size + 7) >> 3))
        return Z_BUF_ERROR;
#endif
    do {
        put = Buf_size - s->bi_valid;
        if (put > bits)
            put = bits;
        s->bi_buf |= (ush)((value & ((1 << put) - 1)) << s->bi_valid);
        s->bi_valid += put;
        _tr_flush_bits(s);
        value >>= put;
        bits -= put;
    } while (bits);
    return Z_OK;
}

/* ========================================================================= */
int ZEXPORT deflateParams(z_streamp strm, int level, int strategy) {
    deflate_state *s;
    compress_func func;
#if defined(Z_TURBO_THREADS) && !defined(FASTEST)
    int full;
#endif

    if (deflateStateCheck(strm)) return Z_STREAM_ERROR;
    s = strm->state;

#ifdef FASTEST
    if (level != 0) level = 1;
#else
    if (level == Z_DEFAULT_COMPRESSION) level = 6;
#endif
    if (level < 0 || level > 9 || strategy < 0 || strategy > Z_FIXED) {
        return Z_STREAM_ERROR;
    }
    func = configuration_table[s->level].func;
#if defined(Z_TURBO_THREADS) && !defined(FASTEST)
    /* whether the stream so far inserted every position in the hash chains */
    full = s->level != 0 && func == deflate_slow &&
           s->strategy != Z_HUFFMAN_ONLY && s->strategy != Z_RLE;
#endif

    if ((strategy != s->strategy || func != configuration_table[level].func) &&
        s->last_flush != -2) {
        /* Flush the last buffer: */
        int err = deflate(strm, Z_BLOCK);
        if (err == Z_STREAM_ERROR)
            return err;
        if (strm->avail_in || (s->strstart - s->block_start) + s->lookahead)
            return Z_BUF_ERROR;
    }
    if (s->level != level) {
        if (s->level == 0 && s->matches != 0) {
            if (s->matches == 1)
                slide_hash(s);
            else
                CLEAR_HASH(s);
            s->matches = 0;
        }
        s->level = level;
        s->max_lazy_match   = configuration_table[level].max_lazy;
        s->good_match       = configuration_table[level].good_length;
        s->nice_match       = configuration_table[level].nice_length;
        s->max_chain_length = configuration_table[level].max_chain;
    }
    s->strategy = strategy;
#if defined(Z_TURBO_THREADS) && !defined(FASTEST)
    if (!full && s->last_flush != -2)
        mt_restart(s, s->mt_abs + s->strstart, 1);
#endif
    return Z_OK;
}

/* ========================================================================= */
int ZEXPORT deflateTune(z_streamp strm, int good_length, int max_lazy,
                        int nice_length, int max_chain) {
    deflate_state *s;

    if (deflateStateCheck(strm)) return Z_STREAM_ERROR;
    s = strm->state;
    s->good_match = (uInt)good_length;
    s->max_lazy_match = (uInt)max_lazy;
    s->nice_match = nice_length;
    s->max_chain_length = (uInt)max_chain;
    return Z_OK;
}

/* =========================================================================
 * For the default windowBits of 15 and memLevel of 8, this function returns a
 * close to exact, as well as small, upper bound on the compressed size. This
 * is an expansion of ~0.03%, plus a small constant.
 *
 * For any setting other than those defaults for windowBits and memLevel, one
 * of two worst case bounds is returned. This is at most an expansion of ~4% or
 * ~13%, plus a small constant.
 *
 * Both the 0.03% and 4% derive from the overhead of stored blocks. The first
 * one is for stored blocks of 16383 bytes (memLevel == 8), whereas the second
 * is for stored blocks of 127 bytes (the worst case memLevel == 1). The
 * expansion results from five bytes of header for each stored block.
 *
 * The larger expansion of 13% results from a window size less than or equal to
 * the symbols buffer size (windowBits <= memLevel + 7). In that case some of
 * the data being compressed may have slid out of the sliding window, impeding
 * a stored block from being emitted. Then the only choice is a fixed or
 * dynamic block, where a fixed block limits the maximum expansion to 9 bits
 * per 8-bit byte, plus 10 bits for every block. The smallest block size for
 * which this can occur is 255 (memLevel == 2).
 *
 * Shifts are used to approximate divisions, for speed.
 */
z_size_t ZEXPORT deflateBound_z(z_streamp strm, z_size_t sourceLen) {
    deflate_state *s;
    z_size_t fixedlen, storelen, wraplen, bound;

    /* upper bound for fixed blocks with 9-bit literals and length 255
       (memLevel == 2, which is the lowest that may not use stored blocks) --
       ~13% overhead plus a small constant */
    fixedlen = sourceLen + (sourceLen >> 3) + (sourceLen >> 8) +
               (sourceLen >> 9) + 4;
    if (fixedlen < sourceLen)
        fixedlen = (z_size_t)-1;

    /* upper bound for stored blocks with length 127 (memLevel == 1) --
       ~4% overhead plus a small constant */
    storelen = sourceLen + (sourceLen >> 5) + (sourceLen >> 7) +
               (sourceLen >> 11) + 7;
    if (storelen < sourceLen)
        storelen = (z_size_t)-1;

    /* if can't get parameters, return larger bound plus a wrapper */
    if (deflateStateCheck(strm)) {
        bound = fixedlen > storelen ? fixedlen : storelen;
        return bound + 18 < bound ? (z_size_t)-1 : bound + 18;
    }

    /* compute wrapper length */
    s = strm->state;
    switch (s->wrap < 0 ? -s->wrap : s->wrap) {
    case 0:                                 /* raw deflate */
        wraplen = 0;
        break;
    case 1:                                 /* zlib wrapper */
        wraplen = 6 + (s->strstart ? 4 : 0);
        break;
#ifdef GZIP
    case 2:                                 /* gzip wrapper */
        wraplen = 18;
        if (s->gzhead != Z_NULL) {          /* user-supplied gzip header */
            Bytef *str;
            if (s->gzhead->extra != Z_NULL)
                wraplen += 2 + s->gzhead->extra_len;
            str = s->gzhead->name;
            if (str != Z_NULL)
                do {
                    wraplen++;
                } while (*str++);
            str = s->gzhead->comment;
            if (str != Z_NULL)
                do {
                    wraplen++;
                } while (*str++);
            if (s->gzhead->hcrc)
                wraplen += 2;
        }
        break;
#endif
    default:                                /* for compiler happiness */
        wraplen = 18;
    }

    /* if not default parameters, return one of the conservative bounds */
    if (s->w_bits != 15 || s->hash_bits != 8 + 7) {
        bound = s->w_bits <= s->hash_bits && s->level ? fixedlen :
                                                        storelen;
        return bound + wraplen < bound ? (z_size_t)-1 : bound + wraplen;
    }

    /* default settings: return tight bound for that case -- ~0.03% overhead
       plus a small constant */
    bound = sourceLen + (sourceLen >> 12) + (sourceLen >> 14) +
            (sourceLen >> 25) + 13 - 6 + wraplen;
    return bound < sourceLen ? (z_size_t)-1 : bound;
}
uLong ZEXPORT deflateBound(z_streamp strm, uLong sourceLen) {
    z_size_t bound = deflateBound_z(strm, sourceLen);
    return (uLong)bound != bound ? (uLong)-1 : (uLong)bound;
}

/* =========================================================================
 * Put a short in the pending buffer. The 16-bit value is put in MSB order.
 * IN assertion: the stream state is correct and there is enough room in
 * pending_buf.
 */
local void putShortMSB(deflate_state *s, uInt b) {
    put_byte(s, (Byte)(b >> 8));
    put_byte(s, (Byte)(b & 0xff));
}

/* =========================================================================
 * Flush as much pending output as possible. All deflate() output, except for
 * some deflate_stored() output, goes through this function so some
 * applications may wish to modify it to avoid allocating a large
 * strm->next_out buffer and copying into it. (See also read_buf()).
 */
local void flush_pending(z_streamp strm) {
    unsigned len;
    deflate_state *s = strm->state;

    _tr_flush_bits(s);
    len = s->pending > strm->avail_out ? strm->avail_out :
                                         (unsigned)s->pending;
    if (len == 0) return;

    zmemcpy(strm->next_out, s->pending_out, len);
    strm->next_out  += len;
    s->pending_out  += len;
    strm->total_out += len;
    strm->avail_out -= len;
    s->pending      -= len;
    if (s->pending == 0) {
        s->pending_out = s->pending_buf;
    }
}

/* ===========================================================================
 * Update the header CRC with the bytes s->pending_buf[beg..s->pending - 1].
 */
#define HCRC_UPDATE(beg) \
    do { \
        if (s->gzhead->hcrc && s->pending > (beg)) \
            strm->adler = crc32_z(strm->adler, s->pending_buf + (beg), \
                                  s->pending - (beg)); \
    } while (0)

/* ========================================================================= */
int ZEXPORT deflate(z_streamp strm, int flush) {
    int old_flush; /* value of flush param for previous deflate call */
    deflate_state *s;

    if (deflateStateCheck(strm) || flush > Z_BLOCK || flush < 0) {
        return Z_STREAM_ERROR;
    }
    s = strm->state;

    if (strm->next_out == Z_NULL ||
        (strm->avail_in != 0 && strm->next_in == Z_NULL) ||
        (s->status == FINISH_STATE && flush != Z_FINISH)) {
        ERR_RETURN(strm, Z_STREAM_ERROR);
    }
    if (strm->avail_out == 0) ERR_RETURN(strm, Z_BUF_ERROR);

    old_flush = s->last_flush;
    s->last_flush = flush;

    /* Flush as much pending output as possible */
    if (s->pending != 0) {
        flush_pending(strm);
        if (strm->avail_out == 0) {
            /* Since avail_out is 0, deflate will be called again with
             * more output space, but possibly with both pending and
             * avail_in equal to zero. There won't be anything to do,
             * but this is not an error situation so make sure we
             * return OK instead of BUF_ERROR at next call of deflate:
             */
            s->last_flush = -1;
            return Z_OK;
        }

    /* Make sure there is something to do and avoid duplicate consecutive
     * flushes. For repeated and useless calls with Z_FINISH, we keep
     * returning Z_STREAM_END instead of Z_BUF_ERROR.
     */
    } else if (strm->avail_in == 0 && RANK(flush) <= RANK(old_flush) &&
               flush != Z_FINISH) {
        ERR_RETURN(strm, Z_BUF_ERROR);
    }

    /* User must not provide more input after the first FINISH: */
    if (s->status == FINISH_STATE && strm->avail_in != 0) {
        ERR_RETURN(strm, Z_BUF_ERROR);
    }

    /* Write the header */
    if (s->status == INIT_STATE && s->wrap == 0)
        s->status = BUSY_STATE;
    if (s->status == INIT_STATE) {
        /* zlib header */
        uInt header = (Z_DEFLATED + ((s->w_bits - 8) << 4)) << 8;
        uInt level_flags;

        if (s->strategy >= Z_HUFFMAN_ONLY || s->level < 2)
            level_flags = 0;
        else if (s->level < 6)
            level_flags = 1;
        else if (s->level == 6)
            level_flags = 2;
        else
            level_flags = 3;
        header |= (level_flags << 6);
        if (s->strstart != 0) header |= PRESET_DICT;
        header += 31 - (header % 31);

        putShortMSB(s, header);

        /* Save the adler32 of the preset dictionary: */
        if (s->strstart != 0) {
            putShortMSB(s, (uInt)(strm->adler >> 16));
            putShortMSB(s, (uInt)(strm->adler & 0xffff));
        }
        strm->adler = adler32(0L, Z_NULL, 0);
        s->status = BUSY_STATE;

        /* Compression must start with an empty pending buffer */
        flush_pending(strm);
        if (s->pending != 0) {
            s->last_flush = -1;
            return Z_OK;
        }
    }
#ifdef GZIP
    if (s->status == GZIP_STATE) {
        /* gzip header */
        strm->adler = crc32(0L, Z_NULL, 0);
        put_byte(s, 31);
        put_byte(s, 139);
        put_byte(s, 8);
        if (s->gzhead == Z_NULL) {
            put_byte(s, 0);
            put_byte(s, 0);
            put_byte(s, 0);
            put_byte(s, 0);
            put_byte(s, 0);
            put_byte(s, s->level == 9 ? 2 :
                     (s->strategy >= Z_HUFFMAN_ONLY || s->level < 2 ?
                      4 : 0));
            put_byte(s, OS_CODE);
            s->status = BUSY_STATE;

            /* Compression must start with an empty pending buffer */
            flush_pending(strm);
            if (s->pending != 0) {
                s->last_flush = -1;
                return Z_OK;
            }
        }
        else {
            put_byte(s, (s->gzhead->text ? 1 : 0) +
                     (s->gzhead->hcrc ? 2 : 0) +
                     (s->gzhead->extra == Z_NULL ? 0 : 4) +
                     (s->gzhead->name == Z_NULL ? 0 : 8) +
                     (s->gzhead->comment == Z_NULL ? 0 : 16)
                     );
            put_byte(s, (Byte)(s->gzhead->time & 0xff));
            put_byte(s, (Byte)((s->gzhead->time >> 8) & 0xff));
            put_byte(s, (Byte)((s->gzhead->time >> 16) & 0xff));
            put_byte(s, (Byte)((s->gzhead->time >> 24) & 0xff));
            put_byte(s, s->level == 9 ? 2 :
                     (s->strategy >= Z_HUFFMAN_ONLY || s->level < 2 ?
                      4 : 0));
            put_byte(s, s->gzhead->os & 0xff);
            if (s->gzhead->extra != Z_NULL) {
                put_byte(s, s->gzhead->extra_len & 0xff);
                put_byte(s, (s->gzhead->extra_len >> 8) & 0xff);
            }
            if (s->gzhead->hcrc)
                strm->adler = crc32_z(strm->adler, s->pending_buf,
                                      s->pending);
            s->gzindex = 0;
            s->status = EXTRA_STATE;
        }
    }
    if (s->status == EXTRA_STATE) {
        if (s->gzhead->extra != Z_NULL) {
            ulg beg = s->pending;   /* start of bytes to update crc */
            ulg left = (s->gzhead->extra_len & 0xffff) - s->gzindex;
            while (s->pending + left > s->pending_buf_size) {
                ulg copy = s->pending_buf_size - s->pending;
                zmemcpy(s->pending_buf + s->pending,
                        s->gzhead->extra + s->gzindex, copy);
                s->pending = s->pending_buf_size;
                HCRC_UPDATE(beg);
                s->gzindex += copy;
                flush_pending(strm);
                if (s->pending != 0) {
                    s->last_flush = -1;
                    return Z_OK;
                }
                beg = 0;
                left -= copy;
            }
            zmemcpy(s->pending_buf + s->pending,
                    s->gzhead->extra + s->gzindex, left);
            s->pending += left;
            HCRC_UPDATE(beg);
            s->gzindex = 0;
        }
        s->status = NAME_STATE;
    }
    if (s->status == NAME_STATE) {
        if (s->gzhead->name != Z_NULL) {
            ulg beg = s->pending;   /* start of bytes to update crc */
            int val;
            do {
                if (s->pending == s->pending_buf_size) {
                    HCRC_UPDATE(beg);
                    flush_pending(strm);
                    if (s->pending != 0) {
                        s->last_flush = -1;
                        return Z_OK;
                    }
                    beg = 0;
                }
                val = s->gzhead->name[s->gzindex++];
                put_byte(s, val);
            } while (val != 0);
            HCRC_UPDATE(beg);
            s->gzindex = 0;
        }
        s->status = COMMENT_STATE;
    }
    if (s->status == COMMENT_STATE) {
        if (s->gzhead->comment != Z_NULL) {
            ulg beg = s->pending;   /* start of bytes to update crc */
            int val;
            do {
                if (s->pending == s->pending_buf_size) {
                    HCRC_UPDATE(beg);
                    flush_pending(strm);
                    if (s->pending != 0) {
                        s->last_flush = -1;
                        return Z_OK;
                    }
                    beg = 0;
                }
                val = s->gzhead->comment[s->gzindex++];
                put_byte(s, val);
            } while (val != 0);
            HCRC_UPDATE(beg);
        }
        s->status = HCRC_STATE;
    }
    if (s->status == HCRC_STATE) {
        if (s->gzhead->hcrc) {
            if (s->pending + 2 > s->pending_buf_size) {
                flush_pending(strm);
                if (s->pending != 0) {
                    s->last_flush = -1;
                    return Z_OK;
                }
            }
            put_byte(s, (Byte)(strm->adler & 0xff));
            put_byte(s, (Byte)((strm->adler >> 8) & 0xff));
            strm->adler = crc32(0L, Z_NULL, 0);
        }
        s->status = BUSY_STATE;

        /* Compression must start with an empty pending buffer */
        flush_pending(strm);
        if (s->pending != 0) {
            s->last_flush = -1;
            return Z_OK;
        }
    }
#endif

    /* Start a new block or continue the current one.
     */
    if (strm->avail_in != 0 || s->lookahead != 0 ||
        (flush != Z_NO_FLUSH && s->status != FINISH_STATE)) {
        block_state bstate;

        bstate = s->level == 0 ? deflate_stored(s, flush) :
                 s->strategy == Z_HUFFMAN_ONLY ? deflate_huff(s, flush) :
                 s->strategy == Z_RLE ? deflate_rle(s, flush) :
                 (*(configuration_table[s->level].func))(s, flush);

        if (bstate == finish_started || bstate == finish_done) {
            s->status = FINISH_STATE;
        }
        if (bstate == need_more || bstate == finish_started) {
            if (strm->avail_out == 0) {
                s->last_flush = -1; /* avoid BUF_ERROR next call, see above */
            }
            return Z_OK;
            /* If flush != Z_NO_FLUSH && avail_out == 0, the next call
             * of deflate should use the same flush parameter to make sure
             * that the flush is complete. So we don't have to output an
             * empty block here, this will be done at next call. This also
             * ensures that for a very small output buffer, we emit at most
             * one empty block.
             */
        }
        if (bstate == block_done) {
            if (flush == Z_PARTIAL_FLUSH) {
                _tr_align(s);
            } else if (flush != Z_BLOCK) { /* FULL_FLUSH or SYNC_FLUSH */
                _tr_stored_block(s, (char*)0, 0L, 0);
                /* For a full flush, this empty block will be recognized
                 * as a special marker by inflate_sync().
                 */
                if (flush == Z_FULL_FLUSH) {
#ifdef Z_TURBO_THREADS
                    Z_U8 at = s->mt_abs + s->strstart;
#endif
                    CLEAR_HASH(s);             /* forget history */
                    if (s->lookahead == 0) {
                        s->strstart = 0;
                        s->block_start = 0L;
                        s->insert = 0;
                    }
#ifdef Z_TURBO_THREADS
                    mt_restart(s, at, 0);      /* history starts here */
#endif
                }
            }
            flush_pending(strm);
            if (strm->avail_out == 0) {
              s->last_flush = -1; /* avoid BUF_ERROR at next call, see above */
              return Z_OK;
            }
        }
    }

    if (flush != Z_FINISH) return Z_OK;
    if (s->wrap <= 0) return Z_STREAM_END;

    /* Write the trailer */
#ifdef GZIP
    if (s->wrap == 2) {
        put_byte(s, (Byte)(strm->adler & 0xff));
        put_byte(s, (Byte)((strm->adler >> 8) & 0xff));
        put_byte(s, (Byte)((strm->adler >> 16) & 0xff));
        put_byte(s, (Byte)((strm->adler >> 24) & 0xff));
        put_byte(s, (Byte)(strm->total_in & 0xff));
        put_byte(s, (Byte)((strm->total_in >> 8) & 0xff));
        put_byte(s, (Byte)((strm->total_in >> 16) & 0xff));
        put_byte(s, (Byte)((strm->total_in >> 24) & 0xff));
    }
    else
#endif
    {
        putShortMSB(s, (uInt)(strm->adler >> 16));
        putShortMSB(s, (uInt)(strm->adler & 0xffff));
    }
    flush_pending(strm);
    /* If avail_out is zero, the application will call deflate again
     * to flush the rest.
     */
    if (s->wrap > 0) s->wrap = -s->wrap; /* write the trailer only once! */
    return s->pending != 0 ? Z_OK : Z_STREAM_END;
}

/* ========================================================================= */
int ZEXPORT deflateEnd(z_streamp strm) {
    int status;

    if (deflateStateCheck(strm)) return Z_STREAM_ERROR;

    status = strm->state->status;

    /* Deallocate in reverse order of allocations: */
#ifdef Z_TURBO_THREADS
    mt_free(strm, strm->state);
#endif
    TRY_FREE(strm, strm->state->pending_buf);
    TRY_FREE(strm, strm->state->head);
    TRY_FREE(strm, strm->state->prev);
    TRY_FREE(strm, strm->state->window);

    ZFREE(strm, strm->state);
    strm->state = Z_NULL;

    return status == BUSY_STATE ? Z_DATA_ERROR : Z_OK;
}

/* =========================================================================
 * Copy the source state to the destination state.
 * To simplify the source, this is not supported for 16-bit MSDOS (which
 * doesn't have enough memory anyway to duplicate compression states).
 */
int ZEXPORT deflateCopy(z_streamp dest, z_streamp source) {
#ifdef MAXSEG_64K
    (void)dest;
    (void)source;
    return Z_STREAM_ERROR;
#else
    deflate_state *ds;
    deflate_state *ss;


    if (deflateStateCheck(source) || dest == Z_NULL) {
        return Z_STREAM_ERROR;
    }

    ss = source->state;

    zmemcpy(dest, source, sizeof(z_stream));

    ds = (deflate_state *) ZALLOC(dest, 1, sizeof(deflate_state));
    if (ds == Z_NULL) return Z_MEM_ERROR;
    zmemzero(ds, sizeof(deflate_state));
    dest->state = (struct internal_state FAR *) ds;
    zmemcpy(ds, ss, sizeof(deflate_state));
    ds->strm = dest;
#ifdef Z_TURBO_THREADS
    ds->mt = Z_NULL;
#endif

    ds->window = (Bytef *) ZALLOC(dest, ds->w_size, 2*sizeof(Byte));
    ds->prev   = (Posf *)  ZALLOC(dest, ds->w_size, sizeof(Pos));
    ds->head   = (Posf *)  ZALLOC(dest, ds->hash_size, sizeof(Pos));
    ds->pending_buf = (uchf *) ZALLOC(dest, ds->lit_bufsize, LIT_BUFS);

    if (ds->window == Z_NULL || ds->prev == Z_NULL || ds->head == Z_NULL ||
        ds->pending_buf == Z_NULL) {
        deflateEnd (dest);
        return Z_MEM_ERROR;
    }
    /* following zmemcpy's do not work for 16-bit MSDOS */
    zmemcpy(ds->window, ss->window, ss->high_water);
    zmemcpy(ds->prev, ss->prev,
            (ss->slid || ss->strstart - ss->insert > ds->w_size ? ds->w_size :
                ss->strstart - ss->insert) * sizeof(Pos));
    zmemcpy(ds->head, ss->head, ds->hash_size * sizeof(Pos));

    ds->pending_out = ds->pending_buf + (ss->pending_out - ss->pending_buf);
    zmemcpy(ds->pending_out, ss->pending_out, ss->pending);
#ifdef LIT_MEM
    ds->d_buf = (ushf *)(ds->pending_buf + (ds->lit_bufsize << 1));
    ds->l_buf = ds->pending_buf + (ds->lit_bufsize << 2);
    zmemcpy(ds->d_buf, ss->d_buf, ss->sym_next * sizeof(ush));
    zmemcpy(ds->l_buf, ss->l_buf, ss->sym_next);
#else
    ds->sym_buf = ds->pending_buf + ds->lit_bufsize;
    zmemcpy(ds->sym_buf, ss->sym_buf, ss->sym_next);
#endif

    ds->l_desc.dyn_tree = ds->dyn_ltree;
    ds->d_desc.dyn_tree = ds->dyn_dtree;
    ds->bl_desc.dyn_tree = ds->bl_tree;

    return Z_OK;
#endif /* MAXSEG_64K */
}

#ifndef FASTEST
/* ===========================================================================
 * Set match_start to the longest match starting at the given string and
 * return its length. Matches shorter or equal to prev_length are discarded,
 * in which case the result is equal to prev_length and match_start is
 * garbage.
 * IN assertions: cur_match is the head of the hash chain for the current
 *   string (strstart) and its distance is <= MAX_DIST, and prev_length >= 1
 * OUT assertion: the match length is not greater than s->lookahead.
 */
#if defined(Z_U8) && !defined(UNALIGNED_OK)
#  if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_ARM64))
#    include <intrin.h>
#  endif
/* The two bytes at p as an unsigned short, in memory order. */
local unsigned short load16(const Bytef *p) {
    unsigned short v;

    zmemcpy(&v, p, 2);
    return v;
}

/* Return the number of leading bytes on which a and b agree, at most
   MAX_MATCH - 2, comparing eight bytes at a time. */
local unsigned count_match(const Bytef *a, const Bytef *b) {
    unsigned n = 0;
    Z_U8 x, y;

    do {
        zmemcpy(&x, a + n, 8);
        zmemcpy(&y, b + n, 8);
        if (x != y) {
#  if defined(__GNUC__) && defined(__BYTE_ORDER__) && \
      __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
            return n + ((unsigned)__builtin_ctzll(x ^ y) >> 3);
#  elif defined(__GNUC__) && defined(__BYTE_ORDER__) && \
        __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
            return n + ((unsigned)__builtin_clzll(x ^ y) >> 3);
#  elif defined(_MSC_VER) && (defined(_M_X64) || defined(_M_ARM64))
            unsigned long i;
            _BitScanForward64(&i, x ^ y);
            return n + (unsigned)(i >> 3);
#  else
            while (a[n] == b[n])
                n++;
            return n;
#  endif
        }
        n += 8;
    } while (n < MAX_MATCH - 2);
    return n;
}
#endif

local uInt longest_match(deflate_state *s, IPos cur_match) {
    unsigned chain_length = s->max_chain_length;/* max hash chain length */
    Bytef *scan = s->window + s->strstart;      /* current string */
    Bytef *match;                               /* matched string */
    int len;                                    /* length of current match */
    int best_len = (int)s->prev_length;         /* best match length so far */
    int nice_match = s->nice_match;             /* stop if match long enough */
    IPos limit = s->strstart > (IPos)MAX_DIST(s) ?
        s->strstart - (IPos)MAX_DIST(s) : NIL;
    /* Stop when cur_match becomes <= limit. To simplify the code,
     * we prevent matches with the string of window index 0.
     */
    Posf *prev = s->prev;
    uInt wmask = s->w_mask;

#ifdef UNALIGNED_OK
    /* Compare two bytes at a time. Note: this is not always beneficial.
     * Try with and without -DUNALIGNED_OK to check.
     */
    Bytef *strend = s->window + s->strstart + MAX_MATCH - 1;
    ush scan_start = *(ushf*)scan;
    ush scan_end   = *(ushf*)(scan + best_len - 1);
#else
#  ifdef Z_U8
    unsigned short scan_start = load16(scan);
    unsigned short scan_end = load16(scan + best_len - 1);
#  else
    Bytef *strend = s->window + s->strstart + MAX_MATCH;
    Byte scan_end1  = scan[best_len - 1];
    Byte scan_end   = scan[best_len];
#  endif
#endif

    /* The code is optimized for HASH_BITS >= 8 and MAX_MATCH-2 multiple of 16.
     * It is easy to get rid of this optimization if necessary.
     */
    Assert(s->hash_bits >= 8 && MAX_MATCH == 258, "Code too clever");

    /* Do not waste too much time if we already have a good match: */
    if (s->prev_length >= s->good_match) {
        chain_length >>= 2;
    }
    /* Do not look for matches beyond the end of the input. This is necessary
     * to make deflate deterministic.
     */
    if ((uInt)nice_match > s->lookahead) nice_match = (int)s->lookahead;

    Assert((ulg)s->strstart <= s->window_size - MIN_LOOKAHEAD,
           "need lookahead");

    do {
        Assert(cur_match < s->strstart, "no future");
        match = s->window + cur_match;

        /* Skip to next match if the match length cannot increase
         * or if the match length is less than 2.  Note that the checks below
         * for insufficient lookahead only occur occasionally for performance
         * reasons.  Therefore uninitialized memory will be accessed, and
         * conditional jumps will be made that depend on those values.
         * However the length of the match is limited to the lookahead, so
         * the output of deflate is not affected by the uninitialized values.
         */
#if (defined(UNALIGNED_OK) && MAX_MATCH == 258)
        /* This code assumes sizeof(unsigned short) == 2. Do not use
         * UNALIGNED_OK if your compiler uses a different size.
         */
        if (*(ushf*)(match + best_len - 1) != scan_end ||
            *(ushf*)match != scan_start) continue;

        /* It is not necessary to compare scan[2] and match[2] since they are
         * always equal when the other bytes match, given that the hash keys
         * are equal and that HASH_BITS >= 8. Compare 2 bytes at a time at
         * strstart + 3, + 5, up to strstart + 257. We check for insufficient
         * lookahead only every 4th comparison; the 128th check will be made
         * at strstart + 257. If MAX_MATCH-2 is not a multiple of 8, it is
         * necessary to put more guard bytes at the end of the window, or
         * to check more often for insufficient lookahead.
         */
        Assert(scan[2] == match[2], "scan[2]?");
        scan++, match++;
        do {
        } while (*(ushf*)(scan += 2) == *(ushf*)(match += 2) &&
                 *(ushf*)(scan += 2) == *(ushf*)(match += 2) &&
                 *(ushf*)(scan += 2) == *(ushf*)(match += 2) &&
                 *(ushf*)(scan += 2) == *(ushf*)(match += 2) &&
                 scan < strend);
        /* The funny "do {}" generates better code on most compilers */

        /* Here, scan <= window + strstart + 257 */
        Assert(scan <= s->window + (unsigned)(s->window_size - 1),
               "wild scan");
        if (*scan == *match) scan++;

        len = (MAX_MATCH - 1) - (int)(strend - scan);
        scan = strend - (MAX_MATCH-1);

#else /* UNALIGNED_OK */

#  ifdef Z_U8
        /* Check bytes best_len - 1 and best_len, and 0 and 1, two at a time.
         * scan[2] and match[2] are equal when the other bytes match, given
         * that the hash keys are equal and that HASH_BITS >= 8. Then compare
         * up to strstart + 257, eight bytes at a time.
         */
        if (load16(match + best_len - 1) != scan_end ||
            load16(match) != scan_start) continue;
        len = 2 + (int)count_match(scan + 2, match + 2);
#  else
        if (match[best_len]     != scan_end  ||
            match[best_len - 1] != scan_end1 ||
            *match              != *scan     ||
            *++match            != scan[1])      continue;

        /* The check at best_len - 1 can be removed because it will be made
         * again later. (This heuristic is not always a win.)
         * It is not necessary to compare scan[2] and match[2] since they
         * are always equal when the other bytes match, given that
         * the hash keys are equal and that HASH_BITS >= 8.
         */
        scan += 2, match++;
        Assert(*scan == *match, "match[2]?");

        /* We check for insufficient lookahead only every 8th comparison;
         * the 256th check will be made at strstart + 258.
         */
        do {
        } while (*++scan == *++match && *++scan == *++match &&
                 *++scan == *++match && *++scan == *++match &&
                 *++scan == *++match && *++scan == *++match &&
                 *++scan == *++match && *++scan == *++match &&
                 scan < strend);

        Assert(scan <= s->window + (unsigned)(s->window_size - 1),
               "wild scan");

        len = MAX_MATCH - (int)(strend - scan);
        scan = strend - MAX_MATCH;
#  endif

#endif /* UNALIGNED_OK */

        if (len > best_len) {
            s->match_start = cur_match;
            best_len = len;
            if (len >= nice_match) break;
#ifdef UNALIGNED_OK
            scan_end = *(ushf*)(scan + best_len - 1);
#elif defined(Z_U8)
            scan_end = load16(scan + best_len - 1);
#else
            scan_end1  = scan[best_len - 1];
            scan_end   = scan[best_len];
#endif
        }
    } while ((cur_match = prev[cur_match & wmask]) > limit
             && --chain_length != 0);

    if ((uInt)best_len <= s->lookahead) return (uInt)best_len;
    return s->lookahead;
}

#else /* FASTEST */

/* ---------------------------------------------------------------------------
 * Optimized version for FASTEST only
 */
local uInt longest_match(deflate_state *s, IPos cur_match) {
    Bytef *scan = s->window + s->strstart;      /* current string */
    Bytef *match;                               /* matched string */
    int len;                                    /* length of current match */
    Bytef *strend = s->window + s->strstart + MAX_MATCH;

    /* The code is optimized for HASH_BITS >= 8 and MAX_MATCH-2 multiple of 16.
     * It is easy to get rid of this optimization if necessary.
     */
    Assert(s->hash_bits >= 8 && MAX_MATCH == 258, "Code too clever");

    Assert((ulg)s->strstart <= s->window_size - MIN_LOOKAHEAD,
           "need lookahead");

    Assert(cur_match < s->strstart, "no future");

    match = s->window + cur_match;

    /* Return failure if the match length is less than 2:
     */
    if (match[0] != scan[0] || match[1] != scan[1]) return MIN_MATCH-1;

    /* The check at best_len - 1 can be removed because it will be made
     * again later. (This heuristic is not always a win.)
     * It is not necessary to compare scan[2] and match[2] since they
     * are always equal when the other bytes match, given that
     * the hash keys are equal and that HASH_BITS >= 8.
     */
    scan += 2, match += 2;
    Assert(*scan == *match, "match[2]?");

    /* We check for insufficient lookahead only every 8th comparison;
     * the 256th check will be made at strstart + 258.
     */
    do {
    } while (*++scan == *++match && *++scan == *++match &&
             *++scan == *++match && *++scan == *++match &&
             *++scan == *++match && *++scan == *++match &&
             *++scan == *++match && *++scan == *++match &&
             scan < strend);

    Assert(scan <= s->window + (unsigned)(s->window_size - 1), "wild scan");

    len = MAX_MATCH - (int)(strend - scan);

    if (len < MIN_MATCH) return MIN_MATCH - 1;

    s->match_start = cur_match;
    return (uInt)len <= s->lookahead ? (uInt)len : s->lookahead;
}

#endif /* FASTEST */

#ifdef ZLIB_DEBUG

#define EQUAL 0
/* result of memcmp for equal strings */

/* ===========================================================================
 * Check that the match at match_start is indeed a match.
 */
local void check_match(deflate_state *s, IPos start, IPos match, int length) {
    /* check that the match is indeed a match */
    Bytef *back = s->window + (int)match, *here = s->window + start;
    IPos len = (IPos)length;
    if (match == (IPos)-1) {
        /* match starts one byte before the current window -- just compare the
           subsequent length-1 bytes */
        back++;
        here++;
        len--;
    }
    if (zmemcmp(back, here, len) != EQUAL) {
        fprintf(stderr, " start %u, match %d, length %d\n",
                start, (int)match, length);
        do {
            fprintf(stderr, "(%02x %02x)", *back++, *here++);
        } while (--len != 0);
        z_error("invalid match");
    }
    if (z_verbose > 1) {
        fprintf(stderr,"\\[%d,%d]", start - match, length);
        do { putc(s->window[start++], stderr); } while (--length != 0);
    }
}
#else
#  define check_match(s, start, match, length)
#endif /* ZLIB_DEBUG */

/* ===========================================================================
 * Flush the current block, with given end-of-file flag.
 * IN assertion: strstart is set to the end of the current match.
 */
#define FLUSH_BLOCK_ONLY(s, last) { \
   _tr_flush_block(s, (s->block_start >= 0L ? \
                   (charf *)&s->window[(unsigned)s->block_start] : \
                   (charf *)Z_NULL), \
                (ulg)((long)s->strstart - s->block_start), \
                (last)); \
   s->block_start = s->strstart; \
   flush_pending(s->strm); \
   Tracev((stderr,"[FLUSH]")); \
}

/* Same but force premature exit if necessary. */
#define FLUSH_BLOCK(s, last) { \
   FLUSH_BLOCK_ONLY(s, last); \
   if (s->strm->avail_out == 0) return (last) ? finish_started : need_more; \
}

/* Maximum stored block length in deflate format (not including header). */
#define MAX_STORED 65535

/* Minimum of a and b. */
#define MIN(a, b) ((a) > (b) ? (b) : (a))

/* ===========================================================================
 * Copy without compression as much as possible from the input stream, return
 * the current block state.
 *
 * In case deflateParams() is used to later switch to a non-zero compression
 * level, s->matches (otherwise unused when storing) keeps track of the number
 * of hash table slides to perform. If s->matches is 1, then one hash table
 * slide will be done when switching. If s->matches is 2, the maximum value
 * allowed here, then the hash table will be cleared, since two or more slides
 * is the same as a clear.
 *
 * deflate_stored() is written to minimize the number of times an input byte is
 * copied. It is most efficient with large input and output buffers, which
 * maximizes the opportunities to have a single copy from next_in to next_out.
 */
local block_state deflate_stored(deflate_state *s, int flush) {
    /* Smallest worthy block size when not flushing or finishing. By default
     * this is 32K. This can be as small as 507 bytes for memLevel == 1. For
     * large input and output buffers, the stored block size will be larger.
     */
    unsigned min_block = (unsigned)(MIN(s->pending_buf_size - 5, s->w_size));

    /* Copy as many min_block or larger stored blocks directly to next_out as
     * possible. If flushing, copy the remaining available input to next_out as
     * stored blocks, if there is enough space.
     */
    int last = 0;
    unsigned len, left, have;
    unsigned used = s->strm->avail_in;
#ifdef Z_TURBO_THREADS
    s->mt_exact = 0;            /* this moves the window its own way */
#endif
    do {
        /* Set len to the maximum size block that we can copy directly with the
         * available input data and output space. Set left to how much of that
         * would be copied from what's left in the window.
         */
        len = MAX_STORED;       /* maximum deflate stored block length */
        have = ((unsigned)s->bi_valid + 42) >> 3;   /* bytes in header */
        if (s->strm->avail_out < have)          /* need room for header */
            break;
            /* maximum stored block length that will fit in avail_out: */
        have = s->strm->avail_out - have;
        left = (unsigned)(s->strstart - s->block_start);    /* window bytes */
        if (len > (ulg)left + s->strm->avail_in)
            len = left + s->strm->avail_in;     /* limit len to the input */
        if (len > have)
            len = have;                         /* limit len to the output */

        /* If the stored block would be less than min_block in length, or if
         * unable to copy all of the available input when flushing, then try
         * copying to the window and the pending buffer instead. Also don't
         * write an empty block when flushing -- deflate() does that.
         */
        if (len < min_block && ((len == 0 && flush != Z_FINISH) ||
                                flush == Z_NO_FLUSH ||
                                len != left + s->strm->avail_in))
            break;

        /* Make a dummy stored block in pending to get the header bytes,
         * including any pending bits. This also updates the debugging counts.
         */
        last = flush == Z_FINISH && len == left + s->strm->avail_in ? 1 : 0;
        _tr_stored_block(s, (char *)0, 0L, last);

        /* Replace the lengths in the dummy stored block with len. */
        s->pending_buf[s->pending - 4] = (Bytef)len;
        s->pending_buf[s->pending - 3] = (Bytef)(len >> 8);
        s->pending_buf[s->pending - 2] = (Bytef)~len;
        s->pending_buf[s->pending - 1] = (Bytef)(~len >> 8);

        /* Write the stored block header bytes. */
        flush_pending(s->strm);

#ifdef ZLIB_DEBUG
        /* Update debugging counts for the data about to be copied. */
        s->compressed_len += len << 3;
        s->bits_sent += len << 3;
#endif

        /* Copy uncompressed bytes from the window to next_out. */
        if (left) {
            if (left > len)
                left = len;
            zmemcpy(s->strm->next_out, s->window + s->block_start, left);
            s->strm->next_out += left;
            s->strm->avail_out -= left;
            s->strm->total_out += left;
            s->block_start += left;
            len -= left;
        }

        /* Copy uncompressed bytes directly from next_in to next_out, updating
         * the check value.
         */
        if (len) {
            read_buf(s->strm, s->strm->next_out, len);
            s->strm->next_out += len;
            s->strm->avail_out -= len;
            s->strm->total_out += len;
        }
    } while (last == 0);

    /* Update the sliding window with the last s->w_size bytes of the copied
     * data, or append all of the copied data to the existing window if less
     * than s->w_size bytes were copied. Also update the number of bytes to
     * insert in the hash tables, in the event that deflateParams() switches to
     * a non-zero compression level.
     */
    used -= s->strm->avail_in;      /* number of input bytes directly copied */
    if (used) {
        /* If any input was used, then no unused input remains in the window,
         * therefore s->block_start == s->strstart.
         */
        if (used >= s->w_size) {    /* supplant the previous history */
            s->matches = 2;         /* clear hash */
            zmemcpy(s->window, s->strm->next_in - s->w_size, s->w_size);
            s->strstart = s->w_size;
            s->insert = s->strstart;
        }
        else {
            if (s->window_size - s->strstart <= used) {
                /* Slide the window down. */
                s->strstart -= s->w_size;
                zmemcpy(s->window, s->window + s->w_size, s->strstart);
                if (s->matches < 2)
                    s->matches++;   /* add a pending slide_hash() */
                if (s->insert > s->strstart)
                    s->insert = s->strstart;
            }
            zmemcpy(s->window + s->strstart, s->strm->next_in - used, used);
            s->strstart += used;
            s->insert += MIN(used, s->w_size - s->insert);
        }
        s->block_start = s->strstart;
    }
    if (s->high_water < s->strstart)
        s->high_water = s->strstart;

    /* If the last block was written to next_out, then done. */
    if (last) {
        s->bi_used = 8;
        return finish_done;
    }

    /* If flushing and all input has been consumed, then done. */
    if (flush != Z_NO_FLUSH && flush != Z_FINISH &&
        s->strm->avail_in == 0 && (long)s->strstart == s->block_start)
        return block_done;

    /* Fill the window with any remaining input. */
    have = (unsigned)(s->window_size - s->strstart);
    if (s->strm->avail_in > have && s->block_start >= (long)s->w_size) {
        /* Slide the window down. */
        s->block_start -= s->w_size;
        s->strstart -= s->w_size;
        zmemcpy(s->window, s->window + s->w_size, s->strstart);
        if (s->matches < 2)
            s->matches++;           /* add a pending slide_hash() */
        have += s->w_size;          /* more space now */
        if (s->insert > s->strstart)
            s->insert = s->strstart;
    }
    if (have > s->strm->avail_in)
        have = s->strm->avail_in;
    if (have) {
        read_buf(s->strm, s->window + s->strstart, have);
        s->strstart += have;
        s->insert += MIN(have, s->w_size - s->insert);
    }
    if (s->high_water < s->strstart)
        s->high_water = s->strstart;

    /* There was not enough avail_out to write a complete worthy or flushed
     * stored block to next_out. Write a stored block to pending instead, if we
     * have enough input for a worthy block, or if flushing and there is enough
     * room for the remaining input as a stored block in the pending buffer.
     */
    have = ((unsigned)s->bi_valid + 42) >> 3;   /* bytes in header */
        /* maximum stored block length that will fit in pending: */
    have = (unsigned)MIN(s->pending_buf_size - have, MAX_STORED);
    min_block = MIN(have, s->w_size);
    left = (unsigned)(s->strstart - s->block_start);
    if (left >= min_block ||
        ((left || flush == Z_FINISH) && flush != Z_NO_FLUSH &&
         s->strm->avail_in == 0 && left <= have)) {
        len = MIN(left, have);
        last = flush == Z_FINISH && s->strm->avail_in == 0 &&
               len == left ? 1 : 0;
        _tr_stored_block(s, (charf *)s->window + s->block_start, len, last);
        s->block_start += len;
        flush_pending(s->strm);
    }

    /* We've done all we can with the available input and output. */
    if (last)
        s->bi_used = 8;
    return last ? finish_started : need_more;
}

/* ===========================================================================
 * Compress as much as possible from the input stream, return the current
 * block state.
 * This function does not perform lazy evaluation of matches and inserts
 * new strings in the dictionary only for unmatched strings or for short
 * matches. It is used only for the fast compression options.
 */
local block_state deflate_fast(deflate_state *s, int flush) {
    IPos hash_head;       /* head of the hash chain */
    int bflush;           /* set if current block must be flushed */

    for (;;) {
        /* Make sure that we always have enough lookahead, except
         * at the end of the input file. We need MAX_MATCH bytes
         * for the next match, plus MIN_MATCH bytes to insert the
         * string following the next match.
         */
        if (s->lookahead < MIN_LOOKAHEAD) {
            fill_window(s);
            if (s->lookahead < MIN_LOOKAHEAD && flush == Z_NO_FLUSH) {
                return need_more;
            }
            if (s->lookahead == 0) break; /* flush the current block */
        }

        /* Insert the string window[strstart .. strstart + 2] in the
         * dictionary, and set hash_head to the head of the hash chain:
         */
        hash_head = NIL;
        if (s->lookahead >= MIN_MATCH) {
            INSERT_STRING(s, s->strstart, hash_head);
        }

        /* Find the longest match, discarding those <= prev_length.
         * At this point we have always match_length < MIN_MATCH
         */
        if (hash_head != NIL && s->strstart - hash_head <= MAX_DIST(s)) {
            /* To simplify the code, we prevent matches with the string
             * of window index 0 (in particular we have to avoid a match
             * of the string with itself at the start of the input file).
             */
            s->match_length = longest_match (s, hash_head);
            /* longest_match() sets match_start */
        }
        if (s->match_length >= MIN_MATCH) {
            check_match(s, s->strstart, s->match_start, (int)s->match_length);

            _tr_tally_dist(s, s->strstart - s->match_start,
                           s->match_length - MIN_MATCH, bflush);

            s->lookahead -= s->match_length;

            /* Insert new strings in the hash table only if the match length
             * is not too large. This saves time but degrades compression.
             */
#ifndef FASTEST
            if (s->match_length <= s->max_insert_length &&
                s->lookahead >= MIN_MATCH) {
                s->match_length--; /* string at strstart already in table */
                do {
                    s->strstart++;
                    INSERT_STRING(s, s->strstart, hash_head);
                    /* strstart never exceeds WSIZE-MAX_MATCH, so there are
                     * always MIN_MATCH bytes ahead.
                     */
                } while (--s->match_length != 0);
                s->strstart++;
            } else
#endif
            {
                s->strstart += s->match_length;
                s->match_length = 0;
                s->ins_h = s->window[s->strstart];
                UPDATE_HASH(s, s->ins_h, s->window[s->strstart + 1]);
#if MIN_MATCH != 3
                Call UPDATE_HASH() MIN_MATCH-3 more times
#endif
                /* If lookahead < MIN_MATCH, ins_h is garbage, but it does not
                 * matter since it will be recomputed at next deflate call.
                 */
            }
        } else {
            /* No match, output a literal byte */
            Tracevv((stderr,"%c", s->window[s->strstart]));
            _tr_tally_lit(s, s->window[s->strstart], bflush);
            s->lookahead--;
            s->strstart++;
        }
        if (bflush) FLUSH_BLOCK(s, 0);
    }
    s->insert = s->strstart < MIN_MATCH-1 ? s->strstart : MIN_MATCH-1;
    if (flush == Z_FINISH) {
        FLUSH_BLOCK(s, 1);
        return finish_done;
    }
    if (s->sym_next)
        FLUSH_BLOCK(s, 0);
    return block_done;
}

#ifndef FASTEST
/* ===========================================================================
 * zlib-turbo: parallel match search for deflate_slow() (levels 4 to 9), with
 * the same output as one thread. Built with -DZ_TURBO_THREADS (POSIX threads)
 * and enabled per stream with deflateSetThreads() in zturbo.h.
 *
 * Why the output can be the same: deflate_slow() inserts every position into
 * the hash chains, so the chain at a position holds all earlier positions
 * with the same hash, whatever matches were chosen before. longest_match()
 * then depends on the window and on two things the parse decides: prev_length,
 * which only filters out matches that are not longer, and whether prev_length
 * reached good_match, which cuts the chain to a quarter. So the longest match
 * at a position over the whole chain, or over its first quarter, is a fact of
 * the input, and a worker can find it ahead of the parse.
 *
 * To search the positions the parse will search, and with the chain length it
 * will use, each worker runs deflate_slow()'s parse over its chunk of input,
 * starting a little before it. The parse resynchronizes quickly, after which
 * the worker searches exactly what the real parse will ask for. The real parse,
 * unchanged, then takes each result whose chain length is the one it needs, and
 * searches itself when there is none (before resynchronization, and at
 * positions whose search could see the end of the available input).
 *
 * Positions are absolute: s->mt_abs is the stream position of window[0]. Each
 * chunk has its own copy of the bytes it needs (MAX_DIST and the warm-up before
 * it, MAX_MATCH after it) and its own hash chains, built from them. A chain cut
 * at the start of the copy only drops positions too far back to be matched.
 */
#ifdef Z_TURBO_THREADS

/* Threads: Windows' own (Vista or later), or POSIX threads elsewhere. */
local void *mt_worker(void *arg);
#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  if !defined(_WIN32_WINNT) || _WIN32_WINNT < 0x0600
#    undef _WIN32_WINNT
#    define _WIN32_WINNT 0x0600
#  endif
#  undef Freq                   /* deflate.h's names, used by windows.h */
#  undef Code
#  undef Dad
#  undef Len
#  include <windows.h>
#  include <process.h>
#  define Freq fc.freq
#  define Code fc.code
#  define Dad  dl.dad
#  define Len  dl.len
typedef SRWLOCK mt_mutex;
typedef CONDITION_VARIABLE mt_cond;
typedef HANDLE mt_thread;
#  define mt_mutex_init(m) InitializeSRWLock(m)
#  define mt_mutex_destroy(m) ((void)(m))
#  define mt_lock(m) AcquireSRWLockExclusive(m)
#  define mt_unlock(m) ReleaseSRWLockExclusive(m)
#  define mt_cond_init(c) InitializeConditionVariable(c)
#  define mt_cond_destroy(c) ((void)(c))
#  define mt_wait(c, m) ((void)SleepConditionVariableSRW(c, m, INFINITE, 0))
#  define mt_signal(c) WakeConditionVariable(c)
#  define mt_broadcast(c) WakeAllConditionVariable(c)
local unsigned __stdcall mt_start(void *arg) {
    mt_worker(arg);
    return 0;
}
/* Start a thread running mt_worker(arg): 0 on success. */
local int mt_create(mt_thread *t, void *arg) {
    uintptr_t h = _beginthreadex(NULL, 0, mt_start, arg, 0, NULL);
    *t = (HANDLE)h;
    return h == 0;
}
local void mt_join(mt_thread t) {
    WaitForSingleObject(t, INFINITE);
    CloseHandle(t);
}
#else
#  include <pthread.h>
typedef pthread_mutex_t mt_mutex;
typedef pthread_cond_t mt_cond;
typedef pthread_t mt_thread;
#  define mt_mutex_init(m) pthread_mutex_init(m, NULL)
#  define mt_mutex_destroy(m) pthread_mutex_destroy(m)
#  define mt_lock(m) pthread_mutex_lock(m)
#  define mt_unlock(m) pthread_mutex_unlock(m)
#  define mt_cond_init(c) pthread_cond_init(c, NULL)
#  define mt_cond_destroy(c) pthread_cond_destroy(c)
#  define mt_wait(c, m) pthread_cond_wait(c, m)
#  define mt_signal(c) pthread_cond_signal(c)
#  define mt_broadcast(c) pthread_cond_broadcast(c)
#  define mt_create(t, arg) pthread_create(t, NULL, mt_worker, arg)
#  define mt_join(t) pthread_join(t, NULL)
#endif

#define MT_CHUNK 262144U        /* positions searched per task */
#define MT_WARM 4096U           /* positions parsed before a chunk to resynchronize */
#define MT_FREE 0
#define MT_QUEUED 1
#define MT_RUNNING 2
#define MT_DONE 3
#define MT_COPY_NEED 0          /* a slot's input bytes: not copied yet, */
#define MT_COPY_BUSY 1          /* being copied, */
#define MT_COPY_DONE 2          /* or in its data */

/* A search at a position: len is the longest match length (2 if none) plus
   MT_FULL or MT_PART for the chain length used, or 0 if not searched. */
#define MT_FULL 0x1000
#define MT_PART 0x2000
typedef struct {
    ush len;
    ush dist;
} mt_res;

typedef struct {
    Z_U8 start;                 /* stream position of the first position */
    Z_U8 data0;                 /* stream position of data[0] */
    Z_U8 floor;                 /* the first position the chains can hold */
    unsigned hist;              /* bytes of data before start */
    unsigned n;                 /* positions searched */
    unsigned size;              /* bytes in data */
    int state;
    Bytef *data;
    const Bytef *src;           /* input bytes still to copy into data */
    unsigned src_at, src_len;   /* where in data, and how many */
    int copy;                   /* MT_COPY_* state of those bytes */
    ush *link;                  /* distance back to the previous position with
                                   the same hash, or 0 for none or too far */
    mt_res *res;                /* the searches at the n positions */
} mt_slot;

struct mt_arg_s;

struct mt_engine_s {
    int threads;
    unsigned nslots;
    mt_slot *slot;
    unsigned first;             /* oldest slot in use */
    unsigned used;              /* slots in use, from first on */
    int ready;                  /* the parse has seen slot[first] done */
    Z_U8 next;                  /* stream position where the next chunk starts */
    /* the parameters of the parse and search, as when the chunks were made */
    unsigned max_chain, good, lazy;
    int nice, strategy;
    unsigned hash_bits, hash_shift, hash_mask, max_dist;
    int **headtab;              /* one hash table per worker, and the parse */
    struct mt_arg_s *args;      /* worker arguments */
    mt_thread *tid;
    mt_mutex lock;
    mt_cond work;
    mt_cond done;
    int quit;
    int started;                /* worker threads running */
    /* block encoding on a worker, while the parse goes on (mt_flush_block) */
    deflate_state *es;          /* writes the block in flight */
    z_stream es_strm;           /* for es->strm->data_type */
    int enc;                    /* MT_FREE, MT_QUEUED, MT_RUNNING or MT_DONE */
    int in_flight;              /* enc is not MT_FREE (the parse's copy) */
    charf *enc_buf;             /* the block's input for a stored block */
    ulg enc_len;
    uchf *sym[2];               /* symbol buffers, filled and written in turn */
    Bytef *snap;                /* copy of the block's input */
};

/* The longest match at data index i, as longest_match() finds it when no
   earlier match is to be beaten, from the candidate at q on, among at most
   count candidates (0: no limit). Returns its length (2 if none) and sets
   *best to its index. */
local int mt_longest(struct mt_engine_s *e, mt_slot *sl, unsigned i, int q,
                     unsigned count, int *best) {
    Bytef *d = sl->data, *scan = d + i;
    ush *link = sl->link;
    unsigned short scan_start = load16(scan);
    unsigned short scan_end = load16(scan + 1);
    int len, best_len = MIN_MATCH - 1, nice = e->nice;
    unsigned seen = 0;
    Z_U8 p = sl->data0 + i;
    Z_U8 limit = p > e->max_dist ? p - e->max_dist : 0;

    *best = -1;
    for (;;) {
        Bytef *match = d + q;
        if (load16(match + best_len - 1) == scan_end &&
            load16(match) == scan_start) {
            len = 2 + (int)count_match(scan + 2, match + 2);
            if (len > best_len) {
                best_len = len;
                *best = q;
                if (len >= nice)
                    break;
                scan_end = load16(scan + best_len - 1);
            }
        }
        if (++seen == count || link[q] == 0)
            break;
        q -= link[q];
        if (sl->data0 + (unsigned)q <= limit)
            break;
    }
    return best_len;
}

/* Build the chains of slot sl, run deflate_slow()'s parse over it, and record
   the searches the parse makes in the chunk. */
local void mt_search(struct mt_engine_s *e, mt_slot *sl, int *headtab) {
    Bytef *d = sl->data;
    ush *link = sl->link;
    unsigned i, end = sl->hist + sl->n, nhash;
    unsigned shift = e->hash_shift, mask = e->hash_mask;
    unsigned full = e->max_chain, part = e->max_chain >> 2;
    unsigned match_length = MIN_MATCH - 1, match_available = 0;
    int match_start = 0;

    /* the hash chains of the copy */
    for (i = 0; i < (1U << e->hash_bits); i++)
        headtab[i] = -1;
    nhash = sl->size - (MIN_MATCH - 1);
    if (nhash > end)
        nhash = end;
    i = 0;
    if (sl->floor > sl->data0) {        /* positions no chain holds */
        unsigned k = sl->floor - sl->data0 < nhash ?
                     (unsigned)(sl->floor - sl->data0) : nhash;
        for (; i < k; i++)
            link[i] = 0;
    }
    for (; i < nhash; i++) {
        unsigned h = (((unsigned)d[i] << (2 * shift)) ^
                      ((unsigned)d[i + 1] << shift) ^ d[i + 2]) & mask;
        int q = headtab[h];
        link[i] = (ush)(q < 0 || i - (unsigned)q > e->max_dist ? 0 : i - (unsigned)q);
        headtab[h] = (int)i;
    }
    zmemzero(sl->res, sl->n * sizeof(mt_res));

    /* deflate_slow(), searching instead of inserting and tallying */
    i = sl->hist > MT_WARM ? sl->hist - MT_WARM : 0;
    while (i < end) {
        int q = link[i] ? (int)(i - link[i]) : -1;
        unsigned prev_length = match_length;

        match_length = MIN_MATCH - 1;
        if (q >= 0 && prev_length < e->lazy && i - (unsigned)q <= e->max_dist) {
            int part_chain = prev_length >= e->good, b;
            int len = mt_longest(e, sl, i, q, part_chain ? part : full, &b);

            if (i >= sl->hist) {
                mt_res *r = sl->res + (i - sl->hist);
                r->len = (ush)(len | (part_chain ? MT_PART : MT_FULL));
                r->dist = (ush)(len > MIN_MATCH - 1 ? i - (unsigned)b : 0);
            }
            if ((unsigned)len > prev_length) {
                match_length = (unsigned)len;
                match_start = b;
            }
            else
                match_length = prev_length;
            if (match_length <= 5 && (e->strategy == Z_FILTERED
#if TOO_FAR <= 32767
                || (match_length == MIN_MATCH &&
                    i - (unsigned)match_start > TOO_FAR)
#endif
                ))
                match_length = MIN_MATCH - 1;
        }
        if (prev_length >= MIN_MATCH && match_length <= prev_length) {
            i += prev_length - 1;       /* past the previous match */
            match_available = 0;
            match_length = MIN_MATCH - 1;
        }
        else {
            match_available = 1;
            i++;
        }
    }
    (void)match_available;
}

typedef struct mt_arg_s {
    struct mt_engine_s *e;
    int k;
} mt_arg;

local void mt_take_data(struct mt_engine_s *e, mt_slot *sl);

local void *mt_worker(void *arg) {
    struct mt_engine_s *e = ((mt_arg *)arg)->e;
    int *headtab = e->headtab[((mt_arg *)arg)->k];

    mt_lock(&e->lock);
    for (;;) {
        mt_slot *sl = NULL;
        unsigned j;
        if (e->enc == MT_QUEUED) {      /* a block to write: first */
            e->enc = MT_RUNNING;
            mt_unlock(&e->lock);
            _tr_flush_block(e->es, e->enc_buf, e->enc_len, 0);
            mt_lock(&e->lock);
            e->enc = MT_DONE;
            mt_broadcast(&e->done);
            continue;
        }
        for (j = 0; j < e->used; j++) {
            mt_slot *c = e->slot + (e->first + j) % e->nslots;
            if (c->state == MT_QUEUED) {
                sl = c;
                break;
            }
        }
        if (sl == NULL) {
            if (e->quit)
                break;
            mt_wait(&e->work, &e->lock);
            continue;
        }
        sl->state = MT_RUNNING;
        mt_take_data(e, sl);
        mt_unlock(&e->lock);
        mt_search(e, sl, headtab);
        mt_lock(&e->lock);
        sl->state = MT_DONE;
        mt_broadcast(&e->done);
    }
    mt_unlock(&e->lock);
    return NULL;
}

/* Wait for the slot to be free of workers, then free it. Called locked. */
local void mt_release(struct mt_engine_s *e, mt_slot *sl) {
    while (sl->state == MT_RUNNING)
        mt_wait(&e->done, &e->lock);
    sl->state = MT_FREE;
}

/* Drop all chunks, and start again at the current position. */
local void mt_reset(deflate_state *s) {
    struct mt_engine_s *e = s->mt;
    unsigned j;

    if (e == Z_NULL)
        return;
    mt_lock(&e->lock);
    for (j = 0; j < e->used; j++)
        mt_release(e, e->slot + (e->first + j) % e->nslots);
    e->first = e->used = 0;
    e->ready = 0;
    e->next = s->mt_abs + s->strstart;
    if (e->next < s->mt_from)
        e->next = s->mt_from;
    e->max_chain = s->max_chain_length;
    e->good = s->good_match;
    e->lazy = s->max_lazy_match;
    e->nice = s->nice_match;
    e->strategy = s->strategy;
    mt_unlock(&e->lock);
}

/* Set up slot sl's data, the bytes at stream positions [from, from + size):
   copy those in the window now, as the parse moves the window, and leave the
   rest, in the input not yet read into it, for the thread that searches the
   slot to copy. */
local void mt_copy(deflate_state *s, mt_slot *sl, Z_U8 from) {
    Z_U8 wend = s->mt_abs + s->strstart + s->lookahead;
    unsigned k = 0;

    if (from < wend) {
        k = (unsigned)(wend - from < sl->size ? wend - from : sl->size);
        zmemcpy(sl->data, s->window + (unsigned)(from - s->mt_abs), k);
    }
    sl->src_at = k;
    sl->src_len = sl->size - k;
    sl->src = sl->src_len ? s->strm->next_in + (unsigned)(from + k - wend) :
                            Z_NULL;
    sl->copy = sl->src_len ? MT_COPY_NEED : MT_COPY_DONE;
}

/* Make sure slot sl has its input bytes, copying them unless another thread
   is. Called and returns locked. */
local void mt_take_data(struct mt_engine_s *e, mt_slot *sl) {
    while (sl->copy != MT_COPY_DONE) {
        if (sl->copy == MT_COPY_NEED) {
            sl->copy = MT_COPY_BUSY;
            mt_unlock(&e->lock);
            zmemcpy(sl->data + sl->src_at, sl->src, sl->src_len);
            mt_lock(&e->lock);
            sl->copy = MT_COPY_DONE;
            mt_broadcast(&e->done);
        }
        else
            mt_wait(&e->done, &e->lock);
    }
}

/* Before deflate() returns, give every queued chunk its input bytes: the
   application may reuse its input buffer after that. */
local void mt_settle(deflate_state *s) {
    struct mt_engine_s *e = s->mt;
    unsigned j;

    mt_lock(&e->lock);
    for (j = 0; j < e->used; j++)
        mt_take_data(e, e->slot + (e->first + j) % e->nslots);
    mt_unlock(&e->lock);
}

/* Queue chunks while there are free slots and input for them. */
local void mt_fill(deflate_state *s) {
    struct mt_engine_s *e = s->mt;
    Z_U8 avail = s->mt_abs + s->strstart + s->lookahead + s->strm->avail_in;

    /* positions with MIN_LOOKAHEAD bytes after them in the input, at least
       4096 of them */
    while (e->used < e->nslots && avail >= e->next + MIN_LOOKAHEAD + 4096) {
        mt_slot *sl = e->slot + (e->first + e->used) % e->nslots;
        Z_U8 start = e->next, lo;
        unsigned n, back = e->max_dist + 8 + MT_WARM;

        n = avail - MIN_LOOKAHEAD - start < MT_CHUNK ?
            (unsigned)(avail - MIN_LOOKAHEAD - start) : MT_CHUNK;
        lo = start > back ? start - back : 0;
        if (lo < s->mt_abs)
            lo = s->mt_abs;
        /* the slot is not seen by the workers until used counts it */
        sl->start = start;
        sl->data0 = lo;
        sl->floor = s->mt_floor;
        sl->hist = (unsigned)(start - lo);
        sl->n = n;
        sl->size = sl->hist + n + MAX_MATCH;
        mt_copy(s, sl, lo);
        mt_lock(&e->lock);
        sl->state = MT_QUEUED;
        e->used++;
        mt_signal(&e->work);
        mt_unlock(&e->lock);
        e->next = start + n;
    }
}

/* Insert the positions skipped since s->mt_ins into the hash chains, up to
   window position end (exclusive), as deflate_slow() would have, and stop
   lagging. Positions more than MAX_DIST before end are left out: no search
   from end on can reach them, and a chain that ends early there ends where
   zlib's would stop. */
local void mt_catch_up(deflate_state *s, unsigned end) {
    unsigned from = s->mt_ins > s->mt_abs ? (unsigned)(s->mt_ins - s->mt_abs) : 0;
    unsigned str, stop = s->strstart + s->lookahead;
    uInt h = 0;

    /* the slides skipped: their order with the insertions skipped does not
       matter, as a slide leaves positions inserted after it unchanged. Two or
       more slides empty the tables of everything before the lag. */
    if (s->mt_slides == 1)
        slide_hash(s);
    else if (s->mt_slides > 1) {
        CLEAR_HASH(s);
        s->slid = 1;
    }
    s->mt_slides = 0;
    if (end > MAX_DIST(s) + 1 && from < end - MAX_DIST(s) - 1)
        from = end - MAX_DIST(s) - 1;
    if (stop >= MIN_MATCH - 1)
        stop -= MIN_MATCH - 1;          /* positions with three bytes */
    else
        stop = 0;
    if (stop > end)
        stop = end;
    for (str = from; str < stop; str++) {
        h = (((uInt)s->window[str] << (2 * s->hash_shift)) ^
             ((uInt)s->window[str + 1] << s->hash_shift) ^
             s->window[str + 2]) & s->hash_mask;
        s->prev[str & s->w_mask] = s->head[h];
        s->head[h] = (Pos)str;
    }
    if (end >= 1 && end + 1 < s->strstart + s->lookahead)
        s->ins_h = (((uInt)s->window[end - 1] << (2 * s->hash_shift)) ^
                    ((uInt)s->window[end] << s->hash_shift) ^
                    s->window[end + 1]) & s->hash_mask;
    (void)h;
    s->mt_ins = s->mt_abs + end;
    s->mt_lag = 0;
}

/* The hash chains start again at stream position at, whose window index is
   now strstart: with no history before it (a full flush, or a dictionary that
   replaced the window), or, if partial, with only some of the positions
   before it (after another compression function or strategy). The window
   positions are counted from here, whatever moved the window before, and the
   chunks start again. */
local void mt_restart(deflate_state *s, Z_U8 at, int partial) {
    s->mt_abs = at - s->strstart;
    s->mt_exact = 1;
    s->mt_lag = 0;
    s->mt_slides = 0;
    if (partial) {
        s->mt_floor = s->mt_abs + 1;
        s->mt_from = at + MAX_DIST(s) + 1;
    }
    else {
        s->mt_floor = at > s->mt_abs ? at : s->mt_abs + 1;
        s->mt_from = 0;
    }
    mt_reset(s);
}

/* The chunk that holds the current position, searched, or Z_NULL if there is
   none (positions before the chunks or too near the end of the available
   input). Frees the chunks behind, queues new ones. */
local mt_slot *mt_slot_at(deflate_state *s) {
    struct mt_engine_s *e = s->mt;
    Z_U8 p = s->mt_abs + s->strstart;
    mt_slot *sl;

    if (e->max_chain != s->max_chain_length || e->good != s->good_match ||
        e->lazy != s->max_lazy_match || e->nice != s->nice_match ||
        e->strategy != s->strategy)
        mt_reset(s);            /* deflateTune() or deflateParams() */

    /* free the chunks the parse has passed, and queue more */
    if (e->used && e->slot[e->first].start + e->slot[e->first].n <= p) {
        mt_lock(&e->lock);
        while (e->used &&
               e->slot[e->first].start + e->slot[e->first].n <= p) {
            mt_release(e, e->slot + e->first);
            e->first = (e->first + 1) % e->nslots;
            e->used--;
            e->ready = 0;
        }
        if (e->used == 0 && e->next < p)
            e->next = p;
        mt_unlock(&e->lock);
    }
    if (e->used < e->nslots)
        mt_fill(s);
    if (e->used == 0 || p < e->slot[e->first].start) {
        return Z_NULL;
    }
    sl = e->slot + e->first;
    if (!e->ready) {
        mt_lock(&e->lock);
        while (sl->state != MT_DONE) {
            if (sl->state == MT_QUEUED) {   /* search it here */
                sl->state = MT_RUNNING;
                mt_take_data(e, sl);
                mt_unlock(&e->lock);
                mt_search(e, sl, e->headtab[e->threads]);
                mt_lock(&e->lock);
                sl->state = MT_DONE;
                mt_broadcast(&e->done);
            }
            else
                mt_wait(&e->done, &e->lock);
        }
        mt_unlock(&e->lock);
        e->ready = 1;
    }
    return sl;
}

/* What INSERT_STRING() would give as hash_head for the current position, from
   the chunk's chains: the previous position with the same hash, or NIL if it
   is at or before window[0] (slid out or index 0) or too far (the chunk keeps
   no link longer than MAX_DIST, and deflate_slow() searches no further) */
local IPos mt_head(deflate_state *s, mt_slot *sl) {
    Z_U8 p = s->mt_abs + s->strstart;
    unsigned link = sl->link[sl->hist + (unsigned)(p - sl->start)];

    if (link == 0 || p - link <= s->mt_abs)
        return NIL;
    return (IPos)(p - link - s->mt_abs);
}

/* longest_match() for deflate_slow(), from the workers' search when it is the
   one needed, or else searched here after bringing the chains up to date */
local uInt mt_result(deflate_state *s, mt_slot *sl, IPos cur_match) {
    mt_res r = sl->res[(unsigned)(s->mt_abs + s->strstart - sl->start)];

    if ((r.len & (s->prev_length >= s->good_match ? MT_PART : MT_FULL)) == 0) {
        IPos h;
        if (s->mt_lag)
            mt_catch_up(s, s->strstart);
        INSERT_STRING(s, s->strstart, h);
        s->mt_ins = s->mt_abs + s->strstart + 1;
        Assert(h == cur_match, "mt_head differs from the chains");
        (void)h;
        return longest_match(s, cur_match);     /* not searched, or not so */
    }
    r.len &= 0x1ff;
    if (r.len > s->prev_length) {
        s->match_start = s->strstart - r.dist;
        return r.len;
    }
    return s->prev_length;
}

/* ===========================================================================
 * Block encoding while the parse goes on. When a block is full, the parse
 * hands its symbols and trees to e->es and goes on in the other symbol buffer,
 * while a worker writes the block with _tr_flush_block(). zlib's own symbol
 * buffer, which shares pending_buf, is not used meanwhile.
 *
 * The output and everything the application can see stay as in zlib: a block
 * is handed off only if the output space is sure to take all of it, so that
 * zlib would have gone on parsing too, and the parse takes the block's output
 * and bits back at the next block or before deflate_slow() returns. The block
 * written by the worker starts with an empty pending_buf, and gets a copy of
 * its input in case a stored block is shorter, since the window can slide.
 */

/* The start of the symbol buffer: the distances first with LIT_MEM. */
#ifdef LIT_MEM
#  define MT_SYM(s) ((uchf *)(s)->d_buf)
#  define MT_SYM_ZLIB(s) ((s)->pending_buf + ((s)->lit_bufsize << 1))
#else
#  define MT_SYM(s) ((s)->sym_buf)
#  define MT_SYM_ZLIB(s) ((s)->pending_buf + (s)->lit_bufsize)
#endif

/* Use the symbol buffer at sym, of 3 * lit_bufsize bytes. */
local void mt_sym_at(deflate_state *s, uchf *sym) {
#ifdef LIT_MEM
    s->d_buf = (ushf *)sym;
    s->l_buf = sym + (s->lit_bufsize << 1);
#else
    s->sym_buf = sym;
#endif
}

/* Move the symbols so far to the buffer at sym, and use it. */
local void mt_sym_move(deflate_state *s, uchf *sym) {
#ifdef LIT_MEM
    zmemcpy(sym, s->d_buf, s->sym_next * sizeof(ush));
    zmemcpy(sym + (s->lit_bufsize << 1), s->l_buf, s->sym_next);
#else
    zmemcpy(sym, s->sym_buf, s->sym_next);
#endif
    mt_sym_at(s, sym);
}

/* Wait for the block in flight, if any, and take its output and bits. */
local void mt_encoded(deflate_state *s) {
    struct mt_engine_s *e = s->mt;
    deflate_state *es = e->es;

    if (!e->in_flight)
        return;
    mt_lock(&e->lock);
    if (e->enc == MT_QUEUED) {          /* no worker free: write it here */
        e->enc = MT_RUNNING;
        mt_unlock(&e->lock);
        _tr_flush_block(es, e->enc_buf, e->enc_len, 0);
        mt_lock(&e->lock);
    }
    else
        while (e->enc != MT_DONE)
            mt_wait(&e->done, &e->lock);
    e->enc = MT_FREE;
    mt_unlock(&e->lock);
    e->in_flight = 0;
    s->pending = es->pending;
    s->bi_buf = es->bi_buf;
    s->bi_valid = es->bi_valid;
    s->bi_used = es->bi_used;
    s->strm->data_type = es->strm->data_type;
#ifdef ZLIB_DEBUG
    s->compressed_len = es->compressed_len;
    s->bits_sent = es->bits_sent;
#endif
    flush_pending(s->strm);
}

/* FLUSH_BLOCK_ONLY() for deflate_slow() with threads: hand the block off. */
local void mt_flush_block(deflate_state *s, int last) {
    struct mt_engine_s *e = s->mt;
    deflate_state *es = e->es;
    ulg len = (ulg)((long)s->strstart - s->block_start), bound;

    mt_encoded(s);
    bound = _tr_block_bound(s);
    if (last || s->pending != 0 || s->strm->avail_out <= bound) {
        FLUSH_BLOCK_ONLY(s, last);      /* as zlib: may fill the output */
        return;
    }
    Assert(s->pending_out == s->pending_buf, "pending_out not reset");
    e->enc_buf = Z_NULL;                /* stored only if not over bound */
    if (s->block_start >= 0L && len + 4 <= bound) {
        zmemcpy(e->snap, s->window + (unsigned)s->block_start, (unsigned)len);
        e->enc_buf = (charf *)e->snap;
    }
    e->enc_len = len;
    es->level = s->level;
    es->strategy = s->strategy;
    es->strm->data_type = s->strm->data_type;
    es->pending_buf = s->pending_buf;
    es->pending_buf_size = s->pending_buf_size;
    es->pending = 0;
    es->lit_bufsize = s->lit_bufsize;
    es->bi_buf = s->bi_buf;
    es->bi_valid = s->bi_valid;
    es->bi_used = s->bi_used;
#ifdef ZLIB_DEBUG
    es->compressed_len = s->compressed_len;
    es->bits_sent = s->bits_sent;
#endif
    mt_sym_at(es, MT_SYM(s));
    _tr_hand_off(s, es);
    mt_sym_at(s, MT_SYM(s) == e->sym[0] ? e->sym[1] : e->sym[0]);
    s->block_start = s->strstart;
    mt_lock(&e->lock);
    e->enc = MT_QUEUED;
    mt_signal(&e->work);
    mt_unlock(&e->lock);
    e->in_flight = 1;
}

local void mt_free(z_streamp strm, deflate_state *s) {
    struct mt_engine_s *e = s->mt;
    unsigned j;
    int k;

    if (e == Z_NULL)
        return;
    if (e->started)
        mt_encoded(s);
    if (e->sym[0] != Z_NULL && MT_SYM(s) != MT_SYM_ZLIB(s))
        mt_sym_move(s, MT_SYM_ZLIB(s));
    if (e->started) {
        mt_lock(&e->lock);
        e->quit = 1;
        mt_broadcast(&e->work);
        mt_unlock(&e->lock);
        for (k = 0; k < e->threads; k++)
            mt_join(e->tid[k]);
        mt_mutex_destroy(&e->lock);
        mt_cond_destroy(&e->work);
        mt_cond_destroy(&e->done);
    }
    if (e->slot != Z_NULL)
        for (j = 0; j < e->nslots; j++) {
            TRY_FREE(strm, e->slot[j].data);
            TRY_FREE(strm, e->slot[j].link);
            TRY_FREE(strm, e->slot[j].res);
        }
    if (e->headtab != Z_NULL)
        for (k = 0; k <= e->threads; k++)
            TRY_FREE(strm, e->headtab[k]);
    TRY_FREE(strm, e->headtab);
    TRY_FREE(strm, e->es);
    TRY_FREE(strm, e->sym[0]);
    TRY_FREE(strm, e->sym[1]);
    TRY_FREE(strm, e->snap);
    TRY_FREE(strm, e->args);
    TRY_FREE(strm, e->slot);
    TRY_FREE(strm, e->tid);
    ZFREE(strm, e);
    s->mt = Z_NULL;
}

/* ========================================================================= */
int ZEXPORT deflateSetThreads(z_streamp strm, int threads) {
    deflate_state *s;
    struct mt_engine_s *e;
    unsigned j, size;
    int k;

    if (deflateStateCheck(strm) || threads < 0 || threads > 256)
        return Z_STREAM_ERROR;
    s = strm->state;
    mt_free(strm, s);
    if (threads <= 1)
        return Z_OK;
    e = (struct mt_engine_s *)ZALLOC(strm, 1, sizeof(struct mt_engine_s));
    if (e == Z_NULL)
        return Z_MEM_ERROR;
    zmemzero(e, sizeof(struct mt_engine_s));
    s->mt = e;
    e->threads = threads;
    e->nslots = 2 * (unsigned)threads + 2;
    e->hash_bits = s->hash_bits;
    e->hash_shift = s->hash_shift;
    e->hash_mask = s->hash_mask;
    e->max_dist = MAX_DIST(s);
    e->slot = (mt_slot *)ZALLOC(strm, e->nslots, sizeof(mt_slot));
    e->tid = (mt_thread *)ZALLOC(strm, threads, sizeof(mt_thread));
    e->headtab = (int **)ZALLOC(strm, threads + 1, sizeof(int *));
    e->args = (mt_arg *)ZALLOC(strm, threads, sizeof(mt_arg));
    if (e->slot == Z_NULL || e->tid == Z_NULL || e->headtab == Z_NULL ||
        e->args == Z_NULL) {
        mt_free(strm, s);
        return Z_MEM_ERROR;
    }
    zmemzero(e->slot, e->nslots * sizeof(mt_slot));
    zmemzero(e->headtab, (threads + 1) * sizeof(int *));
    size = e->max_dist + 8 + MT_WARM + MT_CHUNK + MAX_MATCH;
    for (j = 0; j < e->nslots; j++) {
        e->slot[j].data = (Bytef *)ZALLOC(strm, size, 1);
        e->slot[j].link = (ush *)ZALLOC(strm, size, sizeof(ush));
        e->slot[j].res = (mt_res *)ZALLOC(strm, MT_CHUNK, sizeof(mt_res));
        if (e->slot[j].data == Z_NULL || e->slot[j].link == Z_NULL ||
            e->slot[j].res == Z_NULL) {
            mt_free(strm, s);
            return Z_MEM_ERROR;
        }
    }
    for (k = 0; k <= threads; k++) {
        e->headtab[k] = (int *)ZALLOC(strm, 1U << e->hash_bits, sizeof(int));
        if (e->headtab[k] == Z_NULL) {
            mt_free(strm, s);
            return Z_MEM_ERROR;
        }
    }
    e->es = (deflate_state *)ZALLOC(strm, 1, sizeof(deflate_state));
    e->sym[0] = (uchf *)ZALLOC(strm, s->lit_bufsize, 3);
    e->sym[1] = (uchf *)ZALLOC(strm, s->lit_bufsize, 3);
    e->snap = (Bytef *)ZALLOC(strm, s->w_size, 2);
    if (e->es == Z_NULL || e->sym[0] == Z_NULL || e->sym[1] == Z_NULL ||
        e->snap == Z_NULL) {
        mt_free(strm, s);
        return Z_MEM_ERROR;
    }
    zmemzero(e->es, sizeof(deflate_state));
    e->es->strm = &e->es_strm;
    _tr_init(e->es);
    mt_sym_move(s, e->sym[0]);
    mt_mutex_init(&e->lock);
    mt_cond_init(&e->work);
    mt_cond_init(&e->done);
    e->started = 1;
    for (k = 0; k < threads; k++) {
        e->args[k].e = e;
        e->args[k].k = k;
        if (mt_create(&e->tid[k], e->args + k) != 0) {
            e->threads = k;     /* join those started */
            mt_free(strm, s);
            return Z_MEM_ERROR;
        }
    }
    mt_reset(s);
    return Z_OK;
}

/* deflate_slow()'s block flushes: on a worker with threads */
#  define SLOW_FLUSH_ONLY(s, last) { \
    if (s->mt != Z_NULL) \
        mt_flush_block(s, last); \
    else \
        FLUSH_BLOCK_ONLY(s, last); \
}
#  define SLOW_FLUSH(s, last) { \
    SLOW_FLUSH_ONLY(s, last); \
    if (s->strm->avail_out == 0) return (last) ? finish_started : need_more; \
}
#else
#  define SLOW_FLUSH_ONLY(s, last) FLUSH_BLOCK_ONLY(s, last)
#  define SLOW_FLUSH(s, last) FLUSH_BLOCK(s, last)
#endif /* Z_TURBO_THREADS */

/* ===========================================================================
 * Same as above, but achieves better compression. We use a lazy
 * evaluation for matches: a match is finally adopted only if there is
 * no better match at the next window position.
 */
local block_state deflate_slow_run(deflate_state *s, int flush) {
    IPos hash_head;          /* head of hash chain */
    int bflush;              /* set if current block must be flushed */
#ifdef Z_TURBO_THREADS
    mt_slot *sl;             /* the workers' chunk at this position */
    mt_slot *cur = Z_NULL;   /* the chunk last found by mt_slot_at() */
    Z_U8 cur_end = 0;        /* the stream position after it, 0 if none: the
                                chunks change only in mt_slot_at() */
#endif

    /* Process the input block. */
    for (;;) {
        /* Make sure that we always have enough lookahead, except
         * at the end of the input file. We need MAX_MATCH bytes
         * for the next match, plus MIN_MATCH bytes to insert the
         * string following the next match.
         */
        if (s->lookahead < MIN_LOOKAHEAD) {
            fill_window(s);
            if (s->lookahead < MIN_LOOKAHEAD && flush == Z_NO_FLUSH) {
                return need_more;
            }
            if (s->lookahead == 0) break; /* flush the current block */
        }

        /* Insert the string window[strstart .. strstart + 2] in the
         * dictionary, and set hash_head to the head of the hash chain:
         */
        hash_head = NIL;
#ifdef Z_TURBO_THREADS
        sl = Z_NULL;
        if (s->mt != Z_NULL && s->mt_exact && s->lookahead >= MIN_MATCH) {
            if (s->mt_abs + s->strstart < cur_end)
                sl = cur;               /* still in the same chunk */
            else {
                sl = cur = mt_slot_at(s);
                cur_end = sl != Z_NULL ? sl->start + sl->n : 0;
            }
        }
        if (sl != Z_NULL) {             /* the chunk's chains, no insertion */
            if (!s->mt_lag) {
                s->mt_lag = 1;
                s->mt_ins = s->mt_abs + s->strstart;
            }
            hash_head = mt_head(s, sl);
        }
        else {
            if (s->mt_lag)
                mt_catch_up(s, s->strstart);
#endif
        if (s->lookahead >= MIN_MATCH) {
            INSERT_STRING(s, s->strstart, hash_head);
        }
#ifdef Z_TURBO_THREADS
        }
#endif

        /* Find the longest match, discarding those <= prev_length.
         */
        s->prev_length = s->match_length, s->prev_match = s->match_start;
        s->match_length = MIN_MATCH-1;

        if (hash_head != NIL && s->prev_length < s->max_lazy_match &&
            s->strstart - hash_head <= MAX_DIST(s)) {
            /* To simplify the code, we prevent matches with the string
             * of window index 0 (in particular we have to avoid a match
             * of the string with itself at the start of the input file).
             */
#ifdef Z_TURBO_THREADS
            s->match_length = sl != Z_NULL ? mt_result(s, sl, hash_head) :
                                             longest_match(s, hash_head);
#else
            s->match_length = longest_match (s, hash_head);
#endif
            /* longest_match() sets match_start */

            if (s->match_length <= 5 && (s->strategy == Z_FILTERED
#if TOO_FAR <= 32767
                || (s->match_length == MIN_MATCH &&
                    s->strstart - s->match_start > TOO_FAR)
#endif
                )) {

                /* If prev_match is also MIN_MATCH, match_start is garbage
                 * but we will ignore the current match anyway.
                 */
                s->match_length = MIN_MATCH-1;
            }
        }
        /* If there was a match at the previous step and the current
         * match is not better, output the previous match:
         */
        if (s->prev_length >= MIN_MATCH && s->match_length <= s->prev_length) {
            uInt max_insert = s->strstart + s->lookahead - MIN_MATCH;
            /* Do not insert strings in hash table beyond this. */

            check_match(s, s->strstart - 1, s->prev_match, (int)s->prev_length);

            _tr_tally_dist(s, s->strstart - 1 - s->prev_match,
                           s->prev_length - MIN_MATCH, bflush);

            /* Insert in hash table all strings up to the end of the match.
             * strstart - 1 and strstart are already inserted. If there is not
             * enough lookahead, the last two strings are not inserted in
             * the hash table.
             */
            s->lookahead -= s->prev_length - 1;
            s->prev_length -= 2;
#ifdef Z_TURBO_THREADS
            if (s->mt_lag) {            /* no insertion: skip the match */
                s->strstart += s->prev_length;
                s->prev_length = 0;
            }
            else
#endif
            do {
                if (++s->strstart <= max_insert) {
                    INSERT_STRING(s, s->strstart, hash_head);
                }
            } while (--s->prev_length != 0);
            s->match_available = 0;
            s->match_length = MIN_MATCH-1;
            s->strstart++;

            if (bflush) SLOW_FLUSH(s, 0);

        } else if (s->match_available) {
            /* If there was no match at the previous position, output a
             * single literal. If there was a match but the current match
             * is longer, truncate the previous match to a single literal.
             */
            Tracevv((stderr,"%c", s->window[s->strstart - 1]));
            _tr_tally_lit(s, s->window[s->strstart - 1], bflush);
            if (bflush) {
                SLOW_FLUSH_ONLY(s, 0);
            }
            s->strstart++;
            s->lookahead--;
            if (s->strm->avail_out == 0) return need_more;
        } else {
            /* There is no previous match to compare with, wait for
             * the next step to decide.
             */
            s->match_available = 1;
            s->strstart++;
            s->lookahead--;
        }
    }
    Assert (flush != Z_NO_FLUSH, "no flush?");
    if (s->match_available) {
        Tracevv((stderr,"%c", s->window[s->strstart - 1]));
        _tr_tally_lit(s, s->window[s->strstart - 1], bflush);
        s->match_available = 0;
    }
    s->insert = s->strstart < MIN_MATCH-1 ? s->strstart : MIN_MATCH-1;
    if (flush == Z_FINISH) {
        SLOW_FLUSH(s, 1);
        return finish_done;
    }
    if (s->sym_next)
        SLOW_FLUSH(s, 0);
    return block_done;
}

/* deflate_slow(), leaving the hash chains as zlib does when it returns */
local block_state deflate_slow(deflate_state *s, int flush) {
    block_state bs;
    bs = deflate_slow_run(s, flush);
#ifdef Z_TURBO_THREADS
    if (s->mt_lag)
        mt_catch_up(s, s->strstart - s->insert);
    if (s->mt != Z_NULL) {
        mt_encoded(s);
        mt_settle(s);
    }
#endif
    return bs;
}
#endif /* FASTEST */

#ifndef Z_TURBO_THREADS
/* ========================================================================= */
int ZEXPORT deflateSetThreads(z_streamp strm, int threads) {
    if (deflateStateCheck(strm) || threads < 0)
        return Z_STREAM_ERROR;
    return threads <= 1 ? Z_OK : Z_STREAM_ERROR;
}
#endif

/* ===========================================================================
 * For Z_RLE, simply look for runs of bytes, generate matches only of distance
 * one.  Do not maintain a hash table.  (It will be regenerated if this run of
 * deflate switches away from Z_RLE.)
 */
local block_state deflate_rle(deflate_state *s, int flush) {
    int bflush;             /* set if current block must be flushed */
    uInt prev;              /* byte at distance one to match */
    Bytef *scan, *strend;   /* scan goes up to strend for length of run */

    for (;;) {
        /* Make sure that we always have enough lookahead, except
         * at the end of the input file. We need MAX_MATCH bytes
         * for the longest run, plus one for the unrolled loop.
         */
        if (s->lookahead <= MAX_MATCH) {
            fill_window(s);
            if (s->lookahead <= MAX_MATCH && flush == Z_NO_FLUSH) {
                return need_more;
            }
            if (s->lookahead == 0) break; /* flush the current block */
        }

        /* See how many times the previous byte repeats */
        s->match_length = 0;
        if (s->lookahead >= MIN_MATCH && s->strstart > 0) {
            scan = s->window + s->strstart - 1;
            prev = *scan;
            if (prev == *++scan && prev == *++scan && prev == *++scan) {
                strend = s->window + s->strstart + MAX_MATCH;
                do {
                } while (prev == *++scan && prev == *++scan &&
                         prev == *++scan && prev == *++scan &&
                         prev == *++scan && prev == *++scan &&
                         prev == *++scan && prev == *++scan &&
                         scan < strend);
                s->match_length = MAX_MATCH - (uInt)(strend - scan);
                if (s->match_length > s->lookahead)
                    s->match_length = s->lookahead;
            }
            Assert(scan <= s->window + (uInt)(s->window_size - 1),
                   "wild scan");
        }

        /* Emit match if have run of MIN_MATCH or longer, else emit literal */
        if (s->match_length >= MIN_MATCH) {
            check_match(s, s->strstart, s->strstart - 1, (int)s->match_length);

            _tr_tally_dist(s, 1, s->match_length - MIN_MATCH, bflush);

            s->lookahead -= s->match_length;
            s->strstart += s->match_length;
            s->match_length = 0;
        } else {
            /* No match, output a literal byte */
            Tracevv((stderr,"%c", s->window[s->strstart]));
            _tr_tally_lit(s, s->window[s->strstart], bflush);
            s->lookahead--;
            s->strstart++;
        }
        if (bflush) FLUSH_BLOCK(s, 0);
    }
    s->insert = 0;
    if (flush == Z_FINISH) {
        FLUSH_BLOCK(s, 1);
        return finish_done;
    }
    if (s->sym_next)
        FLUSH_BLOCK(s, 0);
    return block_done;
}

/* ===========================================================================
 * For Z_HUFFMAN_ONLY, do not look for matches.  Do not maintain a hash table.
 * (It will be regenerated if this run of deflate switches away from Huffman.)
 */
local block_state deflate_huff(deflate_state *s, int flush) {
    int bflush;             /* set if current block must be flushed */

    for (;;) {
        /* Make sure that we have a literal to write. */
        if (s->lookahead == 0) {
            fill_window(s);
            if (s->lookahead == 0) {
                if (flush == Z_NO_FLUSH)
                    return need_more;
                break;      /* flush the current block */
            }
        }

        /* Output a literal byte */
        s->match_length = 0;
        Tracevv((stderr,"%c", s->window[s->strstart]));
        _tr_tally_lit(s, s->window[s->strstart], bflush);
        s->lookahead--;
        s->strstart++;
        if (bflush) FLUSH_BLOCK(s, 0);
    }
    s->insert = 0;
    if (flush == Z_FINISH) {
        FLUSH_BLOCK(s, 1);
        return finish_done;
    }
    if (s->sym_next)
        FLUSH_BLOCK(s, 0);
    return block_done;
}
