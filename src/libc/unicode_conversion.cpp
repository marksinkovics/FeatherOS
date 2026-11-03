#include "libc/unicode_conversion.h"

namespace FeatherOS::LibC
{

namespace
{

constexpr uint32_t MaximumCharacter = 0x10FFFF;
constexpr uint32_t FirstSurrogate = 0xD800;
constexpr uint32_t FirstLowSurrogate = 0xDC00;
constexpr uint32_t LastSurrogate = 0xDFFF;
constexpr uint32_t FirstSupplementary = 0x10000;

constexpr bool IsSurrogate(uint32_t character)
{
    return character >= FirstSurrogate && character <= LastSurrogate;
}

// The smallest character each sequence length may encode, so longer forms are overlong.
constexpr uint32_t SmallestOfLength[] = {0, 0, 0x80, 0x800, 0x10000};

void Reset(ConversionState& state)
{
    state = {};
}

} // namespace

size_t DecodeUtf8(ConversionState& state, const char* bytes, size_t count, uint32_t* character)
{
    size_t used = 0;
    while (used < count)
    {
        const auto byte = static_cast<uint8_t>(bytes[used++]);
        if (state.needed == 0)
        {
            if (byte < 0x80)
            {
                *character = byte;
                return byte == 0 ? 0 : used;
            }
            if (byte >= 0xC2 && byte <= 0xDF)
            {
                state = {.value = byte & 0x1Fu, .surrogate = 0, .seen = 1, .needed = 2};
            }
            else if (byte >= 0xE0 && byte <= 0xEF)
            {
                state = {.value = byte & 0x0Fu, .surrogate = 0, .seen = 1, .needed = 3};
            }
            else if (byte >= 0xF0 && byte <= 0xF4)
            {
                state = {.value = byte & 0x07u, .surrogate = 0, .seen = 1, .needed = 4};
            }
            else
            {
                return ConversionInvalid;
            }
            continue;
        }
        if ((byte & 0xC0) != 0x80)
        {
            Reset(state);
            return ConversionInvalid;
        }
        state.value = (state.value << 6) | (byte & 0x3Fu);
        if (++state.seen < state.needed)
        {
            continue;
        }
        const uint32_t value = state.value;
        const uint8_t length = state.needed;
        Reset(state);
        if (value < SmallestOfLength[length] || IsSurrogate(value) || value > MaximumCharacter)
        {
            return ConversionInvalid;
        }
        *character = value;
        return used;
    }
    return ConversionIncomplete;
}

size_t EncodeUtf8(uint32_t character, char* bytes)
{
    if (IsSurrogate(character) || character > MaximumCharacter)
    {
        return ConversionInvalid;
    }
    if (character < 0x80)
    {
        bytes[0] = static_cast<char>(character);
        return 1;
    }
    size_t length = 4;
    if (character < 0x800)
    {
        length = 2;
    }
    else if (character < FirstSupplementary)
    {
        length = 3;
    }
    constexpr uint8_t LeadMarks[] = {0, 0, 0xC0, 0xE0, 0xF0};
    for (size_t index = length - 1; index > 0; --index)
    {
        bytes[index] = static_cast<char>(0x80 | (character & 0x3F));
        character >>= 6;
    }
    bytes[0] = static_cast<char>(LeadMarks[length] | character);
    return length;
}

size_t DecodeUtf8ToUtf16(ConversionState& state, const char* bytes, size_t count, uint16_t* unit)
{
    if (state.surrogate != 0)
    {
        *unit = state.surrogate;
        state.surrogate = 0;
        return ConversionPending;
    }
    uint32_t character = 0;
    const size_t used = DecodeUtf8(state, bytes, count, &character);
    if (used == ConversionInvalid || used == ConversionIncomplete)
    {
        return used;
    }
    if (character < FirstSupplementary)
    {
        *unit = static_cast<uint16_t>(character);
        return used;
    }
    const uint32_t offset = character - FirstSupplementary;
    *unit = static_cast<uint16_t>(FirstSurrogate + (offset >> 10));
    state.surrogate = static_cast<uint16_t>(FirstLowSurrogate + (offset & 0x3FF));
    return used;
}

size_t EncodeUtf16AsUtf8(ConversionState& state, uint16_t unit, char* bytes)
{
    const uint16_t high = state.surrogate;
    state.surrogate = 0;
    if (unit >= FirstSurrogate && unit < FirstLowSurrogate)
    {
        if (high != 0)
        {
            return ConversionInvalid;
        }
        state.surrogate = unit;
        return 0;
    }
    if (unit >= FirstLowSurrogate && unit <= LastSurrogate)
    {
        if (high == 0)
        {
            return ConversionInvalid;
        }
        const uint32_t character =
            FirstSupplementary +
            ((static_cast<uint32_t>(high - FirstSurrogate) << 10) | (unit - FirstLowSurrogate));
        return EncodeUtf8(character, bytes);
    }
    if (high != 0)
    {
        return ConversionInvalid;
    }
    return EncodeUtf8(unit, bytes);
}

} // namespace FeatherOS::LibC
