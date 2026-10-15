/* zturbo.h -- zlib-turbo extensions to the zlib interface
 *
 * zlib.h is unchanged in zlib-turbo, and so is everything it declares. This
 * header declares what zlib-turbo adds. Programs that use these functions
 * need zlib-turbo; all others work with either library.
 */

#ifndef ZTURBO_H
#define ZTURBO_H

#include "zlib.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ZLIB_TURBO 1

/*
     Prepared dictionaries

     deflateSetDictionary() indexes its dictionary for every stream: up to 32K
   hash insertions before the first byte of input. For many short streams
   compressed with the same dictionary, that is most of the work. A prepared
   dictionary is indexed once and then used by any number of streams, also at
   the same time from different threads: it is read-only once made.

     deflateUseDictionary(strm, dict) has the same effect as
   deflateSetDictionary(strm, dictionary, dictLength) with the dictionary dict
   was made from: the same return value and the same compressed output
   afterwards, byte for byte. On a stream that has the windowBits and memLevel
   of the stream the dictionary was prepared on, right after deflateInit2() or
   deflateReset(), it copies the prepared tables instead of indexing; on any
   other stream it calls deflateSetDictionary().
*/

typedef struct z_dict_s z_dict;

ZEXTERN int ZEXPORT deflatePrepareDictionary(z_streamp strm,
                                             const Bytef *dictionary,
                                             uInt dictLength, z_dict **dict);
/*
     Calls deflateSetDictionary(strm, dictionary, dictLength), and if that
   succeeds, also makes a prepared dictionary from the result and returns it in
   *dict. strm must have been just initialized or reset, before any other call
   on it (deflateParams() and deflatePrime() are fine). The dictionary is
   allocated with strm's zalloc and freed with its zfree, and strm can be ended
   before the dictionary is freed.

     Returns Z_OK, or Z_STREAM_ERROR if a parameter is invalid or strm is not
   in that state, or Z_MEM_ERROR if there was not enough memory, in which case
   the dictionary is still set on strm. *dict is Z_NULL unless Z_OK is
   returned.
*/

ZEXTERN int ZEXPORT deflateUseDictionary(z_streamp strm, const z_dict *dict);
/*
     Same as deflateSetDictionary() with the dictionary that dict was made
   from, see above. Returns what deflateSetDictionary() would return.
*/

ZEXTERN void ZEXPORT deflateFreeDictionary(z_dict *dict);
/*
     Frees a prepared dictionary. dict may be Z_NULL.
*/

#ifdef __cplusplus
}
#endif

#endif /* ZTURBO_H */
