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

size_t Utf8Units::Push(uint8_t byte, Utf8Unit out[MaximumUnits])
{
    const bool was_pending = m_decoder.Pending();
    char32_t code[2];
    const size_t produced = m_decoder.Push(byte, code);
    if (produced == 0)
    {
        m_length = was_pending ? m_length : 0;
        m_bytes[m_length++] = byte;
        return 0;
    }
    if (!was_pending)
    {
        out[0] = {{byte, 0, 0, 0}, 1, byte < 0x80};
        return 1;
    }
    if (produced == 1 && !m_decoder.Pending())
    {
        m_bytes[m_length++] = byte;
        out[0] = {{m_bytes[0], m_bytes[1], m_bytes[2], m_bytes[3]}, m_length, true};
        m_length = 0;
        return 1;
    }
    out[0] = {{m_bytes[0], m_bytes[1], m_bytes[2], m_bytes[3]}, m_length, false};
    if (produced == 1)
    {
        m_bytes[0] = byte;
        m_length = 1;
        return 1;
    }
    m_length = 0;
    out[1] = {{byte, 0, 0, 0}, 1, byte < 0x80};
    return 2;
}

size_t Utf8Units::Finish(Utf8Unit* out)
{
    if (!m_decoder.Pending())
    {
        return 0;
    }
    char32_t ignored[2];
    m_decoder.Finish(ignored);
    *out = {{m_bytes[0], m_bytes[1], m_bytes[2], m_bytes[3]}, m_length, false};
    m_length = 0;
    return 1;
}

bool Utf8Units::Pending() const
{
    return m_decoder.Pending();
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

size_t CellCount(const char* text, size_t length)
{
    size_t cells = 0;
    for (size_t offset = 0; offset < length; ++cells)
    {
        size_t used = 0;
        DecodeNext(text + offset, length - offset, &used);
        offset += used;
    }
    return cells;
}

size_t PreviousBoundary(const char* text, size_t length, size_t position)
{
    size_t start = 0;
    size_t offset = 0;
    while (offset < position && offset < length)
    {
        size_t used = 0;
        DecodeNext(text + offset, length - offset, &used);
        start = offset;
        offset += used;
    }
    return start;
}

size_t NextBoundary(const char* text, size_t length, size_t position)
{
    size_t offset = 0;
    while (offset <= position && offset < length)
    {
        size_t used = 0;
        DecodeNext(text + offset, length - offset, &used);
        offset += used;
    }
    return offset;
}

size_t FloorBoundary(const char* text, size_t length, size_t position)
{
    size_t offset = 0;
    while (offset < length)
    {
        size_t used = 0;
        DecodeNext(text + offset, length - offset, &used);
        if (offset + used > position)
        {
            return offset;
        }
        offset += used;
    }
    return length;
}

} // namespace FeatherOS::Text
