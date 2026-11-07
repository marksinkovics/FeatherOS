#ifndef FEATHEROS_LIBC_UNICODE_CONVERSION_H
#define FEATHEROS_LIBC_UNICODE_CONVERSION_H

#include <stddef.h>
#include <stdint.h>

// UTF-8 to and from UTF-32 and UTF-16, one character at a time, for uchar.h. The state carries a
// character split across calls: the UTF-8 bytes read so far, or one half of a surrogate pair.
namespace FeatherOS::LibC
{

struct ConversionState
{
    uint32_t value;
    uint16_t surrogate;
    uint8_t seen;
    uint8_t needed;
};

// The results uchar.h names (size_t)-1, -2 and -3.
constexpr size_t ConversionInvalid = static_cast<size_t>(-1);
constexpr size_t ConversionIncomplete = static_cast<size_t>(-2);
constexpr size_t ConversionPending = static_cast<size_t>(-3);

// Reads at most count bytes. Returns how many completed a character (0 for U+0000),
// ConversionIncomplete if all count bytes were part of one still unfinished, or
// ConversionInvalid for an overlong form, a surrogate, a value past U+10FFFF or a stray byte.
size_t DecodeUtf8(ConversionState& state, const char* bytes, size_t count, uint32_t* character);
// Writes 1 to 4 bytes and returns how many, or ConversionInvalid for a surrogate or a value past
// U+10FFFF.
size_t EncodeUtf8(uint32_t character, char* bytes);
// As DecodeUtf8, but a character past U+FFFF comes back as its high surrogate, and the next
// call returns ConversionPending with the low one without reading any bytes.
size_t DecodeUtf8ToUtf16(ConversionState& state, const char* bytes, size_t count, uint16_t* unit);
// A high surrogate is kept in the state and writes nothing (0); the low one that follows writes
// the whole character.
size_t EncodeUtf16AsUtf8(ConversionState& state, uint16_t unit, char* bytes);

} // namespace FeatherOS::LibC

#endif
