#ifndef FEATHEROS_USER_KEY_DECODER_H
#define FEATHEROS_USER_KEY_DECODER_H

#include <stddef.h>

namespace FeatherOS::User
{

enum class Key : unsigned char
{
    Character,
    // Ctrl with a letter or one of @[\]^_; character holds it in lower case.
    Control,
    Enter,
    Tab,
    BackTab,
    Backspace,
    Escape,
    Up,
    Down,
    Left,
    Right,
    Home,
    End,
    Insert,
    Delete,
    PageUp,
    PageDown,
    Unknown,
};

struct KeyPress
{
    Key key;
    char character;
};

// Decodes what a terminal sends in raw mode, one byte at a time, since a read can end inside an
// escape sequence. Both the CSI (ESC [) and SS3 (ESC O) forms of the cursor keys are understood,
// and Home and End also as ESC [ 1 ~, ESC [ 7 ~, ESC [ 4 ~ and ESC [ 8 ~.
class KeyDecoder
{
public:
    static constexpr size_t MaximumPresses = 2;

    // Returns how many keys byte completed: two when an Escape key is followed by a byte that
    // does not start a sequence.
    size_t Feed(char byte, KeyPress (&presses)[MaximumPresses])
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

    // True while a sequence is incomplete, so the caller can wait briefly for its rest.
    bool Pending() const
    {
        return m_length > 0;
    }

    // Ends an incomplete sequence when no more bytes came: a lone ESC is the Escape key.
    size_t Flush(KeyPress (&presses)[MaximumPresses])
    {
        if (m_length == 0)
        {
            return 0;
        }
        presses[0] = {m_length == 1 ? Key::Escape : Key::Unknown, '\0'};
        m_length = 0;
        return 1;
    }

private:
    char m_sequence[8] = {};
    size_t m_length = 0;

    static KeyPress Single(char byte)
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
            return {Key::Control,
                    base >= 'A' && base <= 'Z' ? static_cast<char>(base + 0x20) : base};
        }
        if (value < 0x7F)
        {
            return {Key::Character, byte};
        }
        return {Key::Unknown, '\0'};
    }

    // The parameter before '~' picks the key; any after ';' (modifiers) are ignored.
    unsigned Parameter() const
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

    Key Final(char byte) const
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
};

} // namespace FeatherOS::User

#endif
