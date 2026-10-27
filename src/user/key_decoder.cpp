#include "key_decoder.h"

namespace FeatherOS::User
{

size_t KeyDecoder::Feed(char byte, KeyPress (&presses)[MaximumPresses])
{
    if (m_length == 0)
    {
        if (byte == '\x1b')
        {
            m_sequence[m_length++] = byte;
            return 0;
        }
        presses[0] = Single(byte);
        return 1;
    }
    if (m_length == 1)
    {
        if (byte == '[' || byte == 'O')
        {
            m_sequence[m_length++] = byte;
            return 0;
        }
        m_length = 0;
        presses[0] = {Key::Escape, '\0'};
        if (byte == '\x1b')
        {
            m_sequence[m_length++] = byte;
            return 1;
        }
        presses[1] = Single(byte);
        return 2;
    }
    const bool final_byte = m_sequence[1] == 'O' || (byte >= 0x40 && byte <= 0x7E);
    if (!final_byte)
    {
        if (byte < 0x20 || byte > 0x3F)
        {
            m_length = 0;
            presses[0] = {Key::Unknown, '\0'};
            return 1;
        }
        // An overlong sequence is still read to its end, so its tail is not typed.
        if (m_length < sizeof(m_sequence))
        {
            m_sequence[m_length] = byte;
        }
        ++m_length;
        return 0;
    }
    presses[0] = {m_length > sizeof(m_sequence) ? Key::Unknown : Final(byte), '\0'};
    m_length = 0;
    return 1;
}

bool KeyDecoder::Pending() const
{
    return m_length > 0;
}

size_t KeyDecoder::Flush(KeyPress (&presses)[MaximumPresses])
{
    if (m_length == 0)
    {
        return 0;
    }
    presses[0] = {m_length == 1 ? Key::Escape : Key::Unknown, '\0'};
    m_length = 0;
    return 1;
}

KeyPress KeyDecoder::Single(char byte)
{
    const auto value = static_cast<unsigned char>(byte);
    if (byte == '\r' || byte == '\n')
    {
        return {Key::Enter, '\0'};
    }
    if (byte == '\t')
    {
        return {Key::Tab, '\0'};
    }
    if (byte == '\x7f' || byte == '\b')
    {
        return {Key::Backspace, '\0'};
    }
    if (value < 0x20)
    {
        const char base = static_cast<char>(value + 0x40);
        return {Key::Control, base >= 'A' && base <= 'Z' ? static_cast<char>(base + 0x20) : base};
    }
    if (value < 0x7F)
    {
        return {Key::Character, byte};
    }
    return {Key::Unknown, '\0'};
}

unsigned KeyDecoder::Parameter() const
{
    unsigned value = 0;
    for (size_t index = 2; index < m_length && m_sequence[index] != ';'; ++index)
    {
        if (m_sequence[index] < '0' || m_sequence[index] > '9')
        {
            return 0;
        }
        value = value * 10 + static_cast<unsigned>(m_sequence[index] - '0');
    }
    return value;
}

Key KeyDecoder::Final(char byte) const
{
    switch (byte)
    {
    case 'A':
        return Key::Up;
    case 'B':
        return Key::Down;
    case 'C':
        return Key::Right;
    case 'D':
        return Key::Left;
    case 'H':
        return Key::Home;
    case 'F':
        return Key::End;
    case 'Z':
        return m_sequence[1] == '[' ? Key::BackTab : Key::Unknown;
    case '~':
        break;
    default:
        return Key::Unknown;
    }
    if (m_sequence[1] != '[')
    {
        return Key::Unknown;
    }
    switch (Parameter())
    {
    case 1:
    case 7:
        return Key::Home;
    case 2:
        return Key::Insert;
    case 3:
        return Key::Delete;
    case 4:
    case 8:
        return Key::End;
    case 5:
        return Key::PageUp;
    case 6:
        return Key::PageDown;
    default:
        return Key::Unknown;
    }
}

} // namespace FeatherOS::User
