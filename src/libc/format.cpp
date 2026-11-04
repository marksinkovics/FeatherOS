#include "format.h"

#include <limits.h>
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

VariadicArguments::VariadicArguments(va_list arguments)
{
    va_copy(list, arguments);
}

VariadicArguments::~VariadicArguments()
{
    va_end(list);
}

bool VariadicArguments::Int(int* value)
{
    *value = va_arg(list, int);
    return true;
}

bool VariadicArguments::Signed(const Specification& specification, int64_t* value)
{
    switch (specification.length)
    {
    case 'l':
        *value = specification.doubled_length ? va_arg(list, long long) : va_arg(list, long);
        break;
    case 'j':
        *value = va_arg(list, intmax_t);
        break;
    case 'z':
    case 't':
        *value = va_arg(list, ptrdiff_t);
        break;
    case 'h':
        *value = specification.doubled_length ? static_cast<signed char>(va_arg(list, int))
                                              : static_cast<short>(va_arg(list, int));
        break;
    default:
        *value = va_arg(list, int);
        break;
    }
    return true;
}

bool VariadicArguments::Unsigned(const Specification& specification, uint64_t* value)
{
    switch (specification.length)
    {
    case 'l':
        *value = specification.doubled_length ? va_arg(list, unsigned long long)
                                              : va_arg(list, unsigned long);
        break;
    case 'j':
        *value = va_arg(list, uintmax_t);
        break;
    case 'z':
    case 't':
        *value = va_arg(list, size_t);
        break;
    case 'h':
        *value = specification.doubled_length
                     ? static_cast<unsigned char>(va_arg(list, unsigned int))
                     : static_cast<unsigned short>(va_arg(list, unsigned int));
        break;
    default:
        *value = va_arg(list, unsigned int);
        break;
    }
    return true;
}

bool VariadicArguments::Character(char* value)
{
    *value = static_cast<char>(va_arg(list, int));
    return true;
}

bool VariadicArguments::String(const char** value)
{
    *value = va_arg(list, const char*);
    return true;
}

bool VariadicArguments::Pointer(const void** value)
{
    *value = va_arg(list, const void*);
    return true;
}

bool VariadicArguments::Float(const Specification& specification, DecodedFloat* value)
{
    *value = specification.length == 'L'
                 ? Decode(LongDoubleFormat, RawBits(va_arg(list, long double)))
                 : Decode(DoubleFormat, RawBits(va_arg(list, double)));
    return true;
}

TaggedArguments::TaggedArguments(const featheros_format_argument* arguments, size_t count)
    : arguments(arguments), count(count)
{
}

// Takes the next argument whatever its type, so that each conversion keeps its own argument
// even after one that did not suit.
const featheros_format_argument* TaggedArguments::Take(int type)
{
    if (next == count)
    {
        return nullptr;
    }
    const featheros_format_argument* argument = &arguments[next++];
    return argument->type == type ? argument : nullptr;
}

bool TaggedArguments::Integer(uint64_t* bits)
{
    if (next == count)
    {
        return false;
    }
    const featheros_format_argument& argument = arguments[next++];
    if (argument.type == FEATHEROS_FORMAT_SIGNED)
    {
        *bits = static_cast<uint64_t>(argument.value.signed_value);
        return true;
    }
    if (argument.type == FEATHEROS_FORMAT_UNSIGNED)
    {
        *bits = argument.value.unsigned_value;
        return true;
    }
    return false;
}

bool TaggedArguments::Int(int* value)
{
    const size_t index = next;
    uint64_t bits = 0;
    if (!Integer(&bits))
    {
        return false;
    }
    // INT_MIN is left out, since a negative width is negated.
    const bool fits =
        arguments[index].type == FEATHEROS_FORMAT_SIGNED
            ? static_cast<int64_t>(bits) > INT_MIN && static_cast<int64_t>(bits) <= INT_MAX
            : bits <= INT_MAX;
    *value = fits ? static_cast<int>(bits) : 0;
    return fits;
}

bool TaggedArguments::Signed(const Specification& specification, int64_t* value)
{
    uint64_t bits = 0;
    if (!Integer(&bits))
    {
        return false;
    }
    if (specification.length == 'h')
    {
        *value = specification.doubled_length ? static_cast<signed char>(bits)
                                              : static_cast<short>(bits);
        return true;
    }
    *value = static_cast<int64_t>(bits);
    return true;
}

bool TaggedArguments::Unsigned(const Specification& specification, uint64_t* value)
{
    uint64_t bits = 0;
    if (!Integer(&bits))
    {
        return false;
    }
    if (specification.length == 'h')
    {
        *value = specification.doubled_length ? static_cast<unsigned char>(bits)
                                              : static_cast<unsigned short>(bits);
        return true;
    }
    *value = bits;
    return true;
}

bool TaggedArguments::Character(char* value)
{
    uint64_t bits = 0;
    if (!Integer(&bits))
    {
        return false;
    }
    *value = static_cast<char>(bits);
    return true;
}

bool TaggedArguments::String(const char** value)
{
    const featheros_format_argument* argument = Take(FEATHEROS_FORMAT_STRING);
    if (argument == nullptr)
    {
        return false;
    }
    *value = argument->value.string;
    return true;
}

bool TaggedArguments::Pointer(const void** value)
{
    const featheros_format_argument* argument = Take(FEATHEROS_FORMAT_POINTER);
    if (argument == nullptr)
    {
        return false;
    }
    *value = argument->value.pointer;
    return true;
}

// Every double is printed as a double, whatever the length modifier.
bool TaggedArguments::Float(const Specification& /*specification*/, DecodedFloat* value)
{
    const featheros_format_argument* argument = Take(FEATHEROS_FORMAT_DOUBLE);
    if (argument == nullptr)
    {
        return false;
    }
    *value = Decode(DoubleFormat, RawBits(argument->value.double_value));
    return true;
}

bool IsDigit(char character)
{
    return character >= '0' && character <= '9';
}

} // namespace FormatDetail
} // namespace FeatherOS::LibC
