#include "format.h"

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

namespace FeatherOS::LibC
{

void BufferSink::Put(char character)
{
    if (length + 1 < capacity)
    {
        buffer[length] = character;
    }
    ++length;
}

void BufferSink::Terminate() const
{
    if (capacity > 0)
    {
        buffer[length < capacity ? length : capacity - 1] = '\0';
    }
}

} // namespace FeatherOS::LibC

namespace FeatherOS::LibC
{

namespace FormatDetail
{

size_t Length(const char* text, int maximum)
{
    size_t length = 0;
    while ((maximum < 0 || length < static_cast<size_t>(maximum)) && text[length] != '\0')
    {
        ++length;
    }
    return length;
}

int64_t SignedArgument(const Specification& specification, va_list* arguments)
{
    switch (specification.length)
    {
    case 'l':
        return specification.doubled_length ? va_arg(*arguments, long long)
                                            : va_arg(*arguments, long);
    case 'j':
        return va_arg(*arguments, intmax_t);
    case 'z':
    case 't':
        return va_arg(*arguments, ptrdiff_t);
    case 'h':
        return specification.doubled_length ? static_cast<signed char>(va_arg(*arguments, int))
                                            : static_cast<short>(va_arg(*arguments, int));
    default:
        return va_arg(*arguments, int);
    }
}

uint64_t UnsignedArgument(const Specification& specification, va_list* arguments)
{
    switch (specification.length)
    {
    case 'l':
        return specification.doubled_length ? va_arg(*arguments, unsigned long long)
                                            : va_arg(*arguments, unsigned long);
    case 'j':
        return va_arg(*arguments, uintmax_t);
    case 'z':
    case 't':
        return va_arg(*arguments, size_t);
    case 'h':
        return specification.doubled_length
                   ? static_cast<unsigned char>(va_arg(*arguments, unsigned int))
                   : static_cast<unsigned short>(va_arg(*arguments, unsigned int));
    default:
        return va_arg(*arguments, unsigned int);
    }
}

bool IsDigit(char character)
{
    return character >= '0' && character <= '9';
}

} // namespace FormatDetail
} // namespace FeatherOS::LibC
