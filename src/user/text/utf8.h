#ifndef FEATHEROS_USER_TEXT_UTF8_H
#define FEATHEROS_USER_TEXT_UTF8_H

#include <stddef.h>
#include <stdint.h>

namespace FeatherOS::Text
{

constexpr char32_t ReplacementCharacter = 0xFFFD;

// Invalid input becomes one U+FFFD per maximal subpart, as Unicode recommends. A byte that
// breaks a sequence ends it and is then read again.
class Utf8Decoder
{
public:
    size_t Push(uint8_t byte, char32_t out[2]);
    size_t Finish(char32_t out[2]);
    bool Pending() const;

private:
    size_t Start(uint8_t byte, char32_t* out);

    char32_t m_code = 0;
    uint8_t m_remaining = 0;
    uint8_t m_lower = 0x80;
    uint8_t m_upper = 0xBF;
};

// Returns 0 with *used 0 for an empty string, else the code point of the first sequence.
char32_t DecodeNext(const char* text, size_t length, size_t* used);

bool IsValidUtf8(const char* text, size_t length);

} // namespace FeatherOS::Text

#endif
