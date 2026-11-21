#include "utf8.h"

namespace FeatherOS::Text
{

namespace
{

struct Lead
{
    uint8_t continuations;
    uint8_t lower;
    uint8_t upper;
    uint32_t bits;
};

// The second byte's range depends on the lead, which is how overlongs, surrogates and values
// past U+10FFFF are caught at the byte where they go wrong.
bool ClassifyLead(uint8_t byte, Lead* lead)
{
    if (byte >= 0xC2 && byte <= 0xDF)
    {
        *lead = {1, 0x80, 0xBF, static_cast<uint32_t>(byte & 0x1FU)};
        return true;
    }
    if (byte >= 0xE0 && byte <= 0xEF)
    {
        *lead = {2, byte == 0xE0 ? uint8_t{0xA0} : uint8_t{0x80},
                 byte == 0xED ? uint8_t{0x9F} : uint8_t{0xBF}, static_cast<uint32_t>(byte & 0x0FU)};
        return true;
    }
    if (byte >= 0xF0 && byte <= 0xF4)
    {
        *lead = {3, byte == 0xF0 ? uint8_t{0x90} : uint8_t{0x80},
                 byte == 0xF4 ? uint8_t{0x8F} : uint8_t{0xBF}, static_cast<uint32_t>(byte & 0x07U)};
        return true;
    }
    return false;
}

char32_t DecodeSequence(const char* text, size_t length, size_t* used, bool* valid)
{
    const uint8_t first = static_cast<uint8_t>(text[0]);
    *valid = true;
    *used = 1;
    if (first < 0x80)
    {
        return first;
    }
    Lead lead{};
    if (!ClassifyLead(first, &lead))
    {
        *valid = false;
        return ReplacementCharacter;
    }
    uint32_t code = lead.bits;
    uint8_t lower = lead.lower;
    uint8_t upper = lead.upper;
    for (uint8_t index = 1; index <= lead.continuations; ++index)
    {
        if (static_cast<size_t>(index) >= length)
        {
            *valid = false;
            return ReplacementCharacter;
        }
        const uint8_t byte = static_cast<uint8_t>(text[index]);
        if (byte < lower || byte > upper)
        {
            *valid = false;
            return ReplacementCharacter;
        }
        code = (code << 6) | (byte & 0x3FU);
        *used = static_cast<size_t>(index) + 1;
        lower = 0x80;
        upper = 0xBF;
    }
    return code;
}

} // namespace

size_t Utf8Decoder::Start(uint8_t byte, char32_t* out)
{
    if (byte < 0x80)
    {
        out[0] = byte;
        return 1;
    }
    Lead lead{};
    if (!ClassifyLead(byte, &lead))
    {
        out[0] = ReplacementCharacter;
        return 1;
    }
    m_code = lead.bits;
    m_remaining = lead.continuations;
    m_lower = lead.lower;
    m_upper = lead.upper;
    return 0;
}

size_t Utf8Decoder::Push(uint8_t byte, char32_t out[2])
{
    if (m_remaining == 0)
    {
        return Start(byte, out);
    }
    if (byte >= m_lower && byte <= m_upper)
    {
        m_code = (m_code << 6) | (byte & 0x3FU);
        m_lower = 0x80;
        m_upper = 0xBF;
        --m_remaining;
        if (m_remaining == 0)
        {
            out[0] = m_code;
            return 1;
        }
        return 0;
    }
    m_remaining = 0;
    out[0] = ReplacementCharacter;
    return 1 + Start(byte, out + 1);
}

size_t Utf8Decoder::Finish(char32_t out[2])
{
    if (m_remaining == 0)
    {
        return 0;
    }
    m_remaining = 0;
    out[0] = ReplacementCharacter;
    return 1;
}

bool Utf8Decoder::Pending() const
{
    return m_remaining != 0;
}

char32_t DecodeNext(const char* text, size_t length, size_t* used)
{
    if (length == 0)
    {
        *used = 0;
        return 0;
    }
    bool valid = true;
    return DecodeSequence(text, length, used, &valid);
}

bool IsValidUtf8(const char* text, size_t length)
{
    size_t offset = 0;
    while (offset < length)
    {
        size_t used = 0;
        bool valid = true;
        DecodeSequence(text + offset, length - offset, &used, &valid);
        if (!valid)
        {
            return false;
        }
        offset += used;
    }
    return true;
}

} // namespace FeatherOS::Text
