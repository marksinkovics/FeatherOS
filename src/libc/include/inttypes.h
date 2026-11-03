#ifndef FEATHEROS_LIBC_INTTYPES_H
#define FEATHEROS_LIBC_INTTYPES_H

#include <stdint.h>
#include <sys/cdefs.h>

// int64_t, intmax_t and intptr_t are long on both targets, with GCC and Clang alike. The fast
// 8- and 16-bit types differ between them (int for GCC, char and short for Clang), so their
// scanf lengths follow the compilers' width macros; printf promotes them to int either way.
#define __FEATHEROS_PRI_64 "l"

#if __INT_FAST8_WIDTH__ == 8
#define __FEATHEROS_SCN_FAST8 "hh"
#else
#define __FEATHEROS_SCN_FAST8 ""
#endif
#if __INT_FAST16_WIDTH__ == 16
#define __FEATHEROS_SCN_FAST16 "h"
#else
#define __FEATHEROS_SCN_FAST16 ""
#endif

#define PRId8 "d"
#define PRIdLEAST8 "d"
#define PRIdFAST8 "d"
#define PRId16 "d"
#define PRIdLEAST16 "d"
#define PRIdFAST16 "d"
#define PRId32 "d"
#define PRIdLEAST32 "d"
#define PRIdFAST32 "d"
#define PRId64 __FEATHEROS_PRI_64 "d"
#define PRIdLEAST64 __FEATHEROS_PRI_64 "d"
#define PRIdFAST64 __FEATHEROS_PRI_64 "d"
#define PRIdMAX "jd"
#define PRIdPTR __FEATHEROS_PRI_64 "d"

#define PRIi8 "i"
#define PRIiLEAST8 "i"
#define PRIiFAST8 "i"
#define PRIi16 "i"
#define PRIiLEAST16 "i"
#define PRIiFAST16 "i"
#define PRIi32 "i"
#define PRIiLEAST32 "i"
#define PRIiFAST32 "i"
#define PRIi64 __FEATHEROS_PRI_64 "i"
#define PRIiLEAST64 __FEATHEROS_PRI_64 "i"
#define PRIiFAST64 __FEATHEROS_PRI_64 "i"
#define PRIiMAX "ji"
#define PRIiPTR __FEATHEROS_PRI_64 "i"

#define PRIo8 "o"
#define PRIoLEAST8 "o"
#define PRIoFAST8 "o"
#define PRIo16 "o"
#define PRIoLEAST16 "o"
#define PRIoFAST16 "o"
#define PRIo32 "o"
#define PRIoLEAST32 "o"
#define PRIoFAST32 "o"
#define PRIo64 __FEATHEROS_PRI_64 "o"
#define PRIoLEAST64 __FEATHEROS_PRI_64 "o"
#define PRIoFAST64 __FEATHEROS_PRI_64 "o"
#define PRIoMAX "jo"
#define PRIoPTR __FEATHEROS_PRI_64 "o"

#define PRIu8 "u"
#define PRIuLEAST8 "u"
#define PRIuFAST8 "u"
#define PRIu16 "u"
#define PRIuLEAST16 "u"
#define PRIuFAST16 "u"
#define PRIu32 "u"
#define PRIuLEAST32 "u"
#define PRIuFAST32 "u"
#define PRIu64 __FEATHEROS_PRI_64 "u"
#define PRIuLEAST64 __FEATHEROS_PRI_64 "u"
#define PRIuFAST64 __FEATHEROS_PRI_64 "u"
#define PRIuMAX "ju"
#define PRIuPTR __FEATHEROS_PRI_64 "u"

#define PRIx8 "x"
#define PRIxLEAST8 "x"
#define PRIxFAST8 "x"
#define PRIx16 "x"
#define PRIxLEAST16 "x"
#define PRIxFAST16 "x"
#define PRIx32 "x"
#define PRIxLEAST32 "x"
#define PRIxFAST32 "x"
#define PRIx64 __FEATHEROS_PRI_64 "x"
#define PRIxLEAST64 __FEATHEROS_PRI_64 "x"
#define PRIxFAST64 __FEATHEROS_PRI_64 "x"
#define PRIxMAX "jx"
#define PRIxPTR __FEATHEROS_PRI_64 "x"

#define PRIX8 "X"
#define PRIXLEAST8 "X"
#define PRIXFAST8 "X"
#define PRIX16 "X"
#define PRIXLEAST16 "X"
#define PRIXFAST16 "X"
#define PRIX32 "X"
#define PRIXLEAST32 "X"
#define PRIXFAST32 "X"
#define PRIX64 __FEATHEROS_PRI_64 "X"
#define PRIXLEAST64 __FEATHEROS_PRI_64 "X"
#define PRIXFAST64 __FEATHEROS_PRI_64 "X"
#define PRIXMAX "jX"
#define PRIXPTR __FEATHEROS_PRI_64 "X"

#define SCNd8                                                                                      \
    "hh"                                                                                           \
    "d"
#define SCNdLEAST8                                                                                 \
    "hh"                                                                                           \
    "d"
#define SCNdFAST8 __FEATHEROS_SCN_FAST8 "d"
#define SCNd16                                                                                     \
    "h"                                                                                            \
    "d"
#define SCNdLEAST16                                                                                \
    "h"                                                                                            \
    "d"
#define SCNdFAST16 __FEATHEROS_SCN_FAST16 "d"
#define SCNd32 "d"
#define SCNdLEAST32 "d"
#define SCNdFAST32 "d"
#define SCNd64 __FEATHEROS_PRI_64 "d"
#define SCNdLEAST64 __FEATHEROS_PRI_64 "d"
#define SCNdFAST64 __FEATHEROS_PRI_64 "d"
#define SCNdMAX "jd"
#define SCNdPTR __FEATHEROS_PRI_64 "d"

#define SCNi8                                                                                      \
    "hh"                                                                                           \
    "i"
#define SCNiLEAST8                                                                                 \
    "hh"                                                                                           \
    "i"
#define SCNiFAST8 __FEATHEROS_SCN_FAST8 "i"
#define SCNi16                                                                                     \
    "h"                                                                                            \
    "i"
#define SCNiLEAST16                                                                                \
    "h"                                                                                            \
    "i"
#define SCNiFAST16 __FEATHEROS_SCN_FAST16 "i"
#define SCNi32 "i"
#define SCNiLEAST32 "i"
#define SCNiFAST32 "i"
#define SCNi64 __FEATHEROS_PRI_64 "i"
#define SCNiLEAST64 __FEATHEROS_PRI_64 "i"
#define SCNiFAST64 __FEATHEROS_PRI_64 "i"
#define SCNiMAX "ji"
#define SCNiPTR __FEATHEROS_PRI_64 "i"

#define SCNo8                                                                                      \
    "hh"                                                                                           \
    "o"
#define SCNoLEAST8                                                                                 \
    "hh"                                                                                           \
    "o"
#define SCNoFAST8 __FEATHEROS_SCN_FAST8 "o"
#define SCNo16                                                                                     \
    "h"                                                                                            \
    "o"
#define SCNoLEAST16                                                                                \
    "h"                                                                                            \
    "o"
#define SCNoFAST16 __FEATHEROS_SCN_FAST16 "o"
#define SCNo32 "o"
#define SCNoLEAST32 "o"
#define SCNoFAST32 "o"
#define SCNo64 __FEATHEROS_PRI_64 "o"
#define SCNoLEAST64 __FEATHEROS_PRI_64 "o"
#define SCNoFAST64 __FEATHEROS_PRI_64 "o"
#define SCNoMAX "jo"
#define SCNoPTR __FEATHEROS_PRI_64 "o"

#define SCNu8                                                                                      \
    "hh"                                                                                           \
    "u"
#define SCNuLEAST8                                                                                 \
    "hh"                                                                                           \
    "u"
#define SCNuFAST8 __FEATHEROS_SCN_FAST8 "u"
#define SCNu16                                                                                     \
    "h"                                                                                            \
    "u"
#define SCNuLEAST16                                                                                \
    "h"                                                                                            \
    "u"
#define SCNuFAST16 __FEATHEROS_SCN_FAST16 "u"
#define SCNu32 "u"
#define SCNuLEAST32 "u"
#define SCNuFAST32 "u"
#define SCNu64 __FEATHEROS_PRI_64 "u"
#define SCNuLEAST64 __FEATHEROS_PRI_64 "u"
#define SCNuFAST64 __FEATHEROS_PRI_64 "u"
#define SCNuMAX "ju"
#define SCNuPTR __FEATHEROS_PRI_64 "u"

#define SCNx8                                                                                      \
    "hh"                                                                                           \
    "x"
#define SCNxLEAST8                                                                                 \
    "hh"                                                                                           \
    "x"
#define SCNxFAST8 __FEATHEROS_SCN_FAST8 "x"
#define SCNx16                                                                                     \
    "h"                                                                                            \
    "x"
#define SCNxLEAST16                                                                                \
    "h"                                                                                            \
    "x"
#define SCNxFAST16 __FEATHEROS_SCN_FAST16 "x"
#define SCNx32 "x"
#define SCNxLEAST32 "x"
#define SCNxFAST32 "x"
#define SCNx64 __FEATHEROS_PRI_64 "x"
#define SCNxLEAST64 __FEATHEROS_PRI_64 "x"
#define SCNxFAST64 __FEATHEROS_PRI_64 "x"
#define SCNxMAX "jx"
#define SCNxPTR __FEATHEROS_PRI_64 "x"

__BEGIN_DECLS

typedef struct
{
    intmax_t quot;
    intmax_t rem;
} imaxdiv_t;

intmax_t imaxabs(intmax_t value);
imaxdiv_t imaxdiv(intmax_t numerator, intmax_t denominator);
intmax_t strtoimax(const char* text, char** end, int base);
uintmax_t strtoumax(const char* text, char** end, int base);

__END_DECLS

#endif
