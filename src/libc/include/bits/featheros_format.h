#ifndef FEATHEROS_LIBC_BITS_FEATHEROS_FORMAT_H
#define FEATHEROS_LIBC_BITS_FEATHEROS_FORMAT_H

// Part of featheros.h, on its own so that the printf engine's host tests can include it.

#define FEATHEROS_FORMAT_SIGNED 0
#define FEATHEROS_FORMAT_UNSIGNED 1
#define FEATHEROS_FORMAT_DOUBLE 2
#define FEATHEROS_FORMAT_STRING 3
#define FEATHEROS_FORMAT_POINTER 4

// One argument of featheros_format, tagged with its type in place of printf's va_list.
struct featheros_format_argument
{
    int type;
    union
    {
        long long signed_value;
        unsigned long long unsigned_value;
        double double_value;
        const char* string;
        const void* pointer;
    } value;
};

#endif
