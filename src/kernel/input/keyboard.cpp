#include "keyboard.h"

namespace FeatherOS::Input
{
namespace
{

struct Sequence
{
    uint16_t code;
    const char* bytes;
};

constexpr Sequence Sequences[] = {
    {KeyCode::Up, "\x1b[A"},        {KeyCode::Down, "\x1b[B"},    {KeyCode::Right, "\x1b[C"},
    {KeyCode::Left, "\x1b[D"},      {KeyCode::Home, "\x1b[H"},    {KeyCode::End, "\x1b[F"},
    {KeyCode::Insert, "\x1b[2~"},   {KeyCode::Delete, "\x1b[3~"}, {KeyCode::PageUp, "\x1b[5~"},
    {KeyCode::PageDown, "\x1b[6~"},
};

size_t Copy(const char* text, char (&bytes)[Keyboard::MaximumKeyBytes])
{
    size_t length = 0;
    while (text[length] != '\0' && length < Keyboard::MaximumKeyBytes)
    {
        bytes[length] = text[length];
        ++length;
    }
    return length;
}

} // namespace

char Keyboard::Character(uint16_t code) const
{
    const bool shift = m_left_shift || m_right_shift;
    // Rows of the US layout, indexed from each row's first key code.
    static constexpr char number_row[] = "1234567890-=";
    static constexpr char shifted_number_row[] = "!@#$%^&*()_+";
    static constexpr char top_row[] = "qwertyuiop[]";
    static constexpr char shifted_top_row[] = "QWERTYUIOP{}";
    static constexpr char home_row[] = "asdfghjkl;'`";
    static constexpr char shifted_home_row[] = "ASDFGHJKL:\"~";
    static constexpr char bottom_row[] = "\\zxcvbnm,./";
    static constexpr char shifted_bottom_row[] = "|ZXCVBNM<>?";
    char character = '\0';
    if (code >= 2 && code <= 13)
    {
        character = (shift ? shifted_number_row : number_row)[code - 2];
    }
    else if (code >= 16 && code <= 27)
    {
        character = (shift ? shifted_top_row : top_row)[code - 16];
    }
    else if (code >= 30 && code <= 41)
    {
        character = (shift ? shifted_home_row : home_row)[code - 30];
    }
    else if (code >= 43 && code <= 53)
    {
        character = (shift ? shifted_bottom_row : bottom_row)[code - 43];
    }
    else if (code == 57)
    {
        character = ' ';
    }
    if (m_caps_lock && character >= 'a' && character <= 'z')
    {
        character = static_cast<char>(character - 'a' + 'A');
    }
    else if (m_caps_lock && character >= 'A' && character <= 'Z')
    {
        character = static_cast<char>(character - 'A' + 'a');
    }
    return character;
}

size_t Keyboard::Press(uint16_t code, bool pressed, char (&bytes)[MaximumKeyBytes])
{
    switch (code)
    {
    case KeyCode::LeftShift:
        m_left_shift = pressed;
        return 0;
    case KeyCode::RightShift:
        m_right_shift = pressed;
        return 0;
    case KeyCode::LeftControl:
        m_left_control = pressed;
        return 0;
    case KeyCode::RightControl:
        m_right_control = pressed;
        return 0;
    case KeyCode::CapsLock:
        m_caps_lock = m_caps_lock != (pressed && !m_caps_lock_held);
        m_caps_lock_held = pressed;
        return 0;
    default:
        break;
    }
    if (!pressed)
    {
        return 0;
    }
    switch (code)
    {
    case KeyCode::Escape:
        return Copy("\x1b", bytes);
    case KeyCode::Backspace:
        return Copy("\x7f", bytes);
    case KeyCode::Tab:
        return Copy(m_left_shift || m_right_shift ? "\x1b[Z" : "\t", bytes);
    case KeyCode::Enter:
    case KeyCode::KeypadEnter:
        return Copy("\r", bytes);
    default:
        break;
    }
    for (const Sequence& sequence : Sequences)
    {
        if (sequence.code == code)
        {
            return Copy(sequence.bytes, bytes);
        }
    }
    char character = Character(code);
    if (character == '\0')
    {
        return 0;
    }
    if (m_left_control || m_right_control)
    {
        // Ctrl keeps the low five bits, as on a terminal: Ctrl+A is 1 and Ctrl+[ is ESC.
        if (character >= 'a' && character <= 'z')
        {
            character = static_cast<char>(character - 'a' + 'A');
        }
        if (character < '@' || character > '_')
        {
            return 0;
        }
        character = static_cast<char>(character & 0x1F);
        if (character == '\0')
        {
            return 0;
        }
    }
    bytes[0] = character;
    return 1;
}

} // namespace FeatherOS::Input
