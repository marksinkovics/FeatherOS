#ifndef FEATHEROS_LIBC_UCHAR_H
#define FEATHEROS_LIBC_UCHAR_H

#include <stddef.h>
#include <stdint.h>
#include <sys/cdefs.h>

// Multibyte text is always UTF-8, whatever the locale.
typedef struct
{
    unsigned int __value;
    unsigned short __surrogate;
    unsigned char __seen;
    unsigned char __needed;
} mbstate_t;

#ifndef __cplusplus
typedef uint_least16_t char16_t;
typedef uint_least32_t char32_t;
#endif

__BEGIN_DECLS

size_t mbrtoc16(char16_t* unit, const char* bytes, size_t count, mbstate_t* state);
size_t c16rtomb(char* bytes, char16_t unit, mbstate_t* state);
size_t mbrtoc32(char32_t* character, const char* bytes, size_t count, mbstate_t* state);
size_t c32rtomb(char* bytes, char32_t character, mbstate_t* state);

__END_DECLS

#endif
