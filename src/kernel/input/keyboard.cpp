#include "keyboard.h"

#include "kernel/input/input.h"

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

// The keypad's key codes: the operators always, the rest as digits with Num Lock on.
char KeypadCharacter(uint16_t code, bool num_lock)
{
    switch (code)
    {
    case 55:
        return '*';
    case 74:
        return '-';
    case 78:
        return '+';
    case 98:
        return '/';
    default:
        break;
    }
    if (!num_lock)
    {
        return '\0';
    }
    static constexpr char digits[] = {'7',  '8', '9', '\0', '4', '5', '6',
                                      '\0', '1', '2', '3',  '0', '.'};
    return code >= 71 && code <= 83 ? digits[code - 71] : '\0';
}

// What the keypad's digit keys are with Num Lock off.
uint16_t KeypadNavigation(uint16_t code)
{
    switch (code)
    {
    case 71:
        return KeyCode::Home;
    case 72:
        return KeyCode::Up;
    case 73:
        return KeyCode::PageUp;
    case 75:
        return KeyCode::Left;
    case 77:
        return KeyCode::Right;
    case 79:
        return KeyCode::End;
    case 80:
        return KeyCode::Down;
    case 81:
        return KeyCode::PageDown;
    case 82:
        return KeyCode::Insert;
    case 83:
        return KeyCode::Delete;
    default:
        return 0;
    }
}

bool IsKeypadDigit(uint16_t code)
{
    return code >= 71 && code <= 83 && code != 74 && code != 78;
}

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
    else if (code == 55 || code == 98 || (code >= 71 && code <= 83))
    {
        character = KeypadCharacter(code, Input::NumLock());
    }
    const bool caps_lock = Input::CapsLock();
    if (caps_lock && character >= 'a' && character <= 'z')
    {
        character = static_cast<char>(character - 'a' + 'A');
    }
    else if (caps_lock && character >= 'A' && character <= 'Z')
    {
        character = static_cast<char>(character - 'A' + 'a');
    }
    return character;
}

KeyTransition Keyboard::Track(uint16_t code, bool pressed)
{
    if (code >= TrackedKeys)
    {
        return KeyTransition::None;
    }
    uint64_t& word = m_held[code / 64];
    const uint64_t bit = uint64_t{1} << (code % 64);
    const bool held = (word & bit) != 0;
    if (pressed)
    {
        word |= bit;
        return held ? KeyTransition::Repeat : KeyTransition::Press;
    }
    word &= ~bit;
    return held ? KeyTransition::Release : KeyTransition::None;
}

uint8_t Keyboard::Modifiers() const
{
    uint8_t modifiers = 0;
    if (m_left_shift || m_right_shift)
    {
        modifiers |= Modifier::Shift;
    }
    if (m_left_control || m_right_control)
    {
        modifiers |= Modifier::Control;
    }
    if (m_left_alt || m_right_alt)
    {
        modifiers |= Modifier::Alt;
    }
    if (m_left_meta || m_right_meta)
    {
        modifiers |= Modifier::Super;
    }
    if (Input::CapsLock())
    {
        modifiers |= Modifier::CapsLock;
    }
    return modifiers;
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
    case KeyCode::LeftAlt:
        m_left_alt = pressed;
        return 0;
    case KeyCode::RightAlt:
        m_right_alt = pressed;
        return 0;
    case KeyCode::LeftMeta:
        m_left_meta = pressed;
        return 0;
    case KeyCode::RightMeta:
        m_right_meta = pressed;
        return 0;
    case KeyCode::CapsLock:
        if (pressed && !m_caps_lock_held)
        {
            Input::ToggleCapsLock();
        }
        m_caps_lock_held = pressed;
        return 0;
    case KeyCode::NumLock:
        if (pressed && !m_num_lock_held)
        {
            Input::ToggleNumLock();
        }
        m_num_lock_held = pressed;
        return 0;
    default:
        break;
    }
    if (!pressed)
    {
        return 0;
    }
    if (code >= KeyCode::F1 && code <= KeyCode::F4)
    {
        if (m_left_alt || m_right_alt)
        {
            m_console_switch = code - KeyCode::F1;
        }
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
    if (IsKeypadDigit(code) && !Input::NumLock())
    {
        code = KeypadNavigation(code);
        if (code == 0)
        {
            return 0;
        }
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

bool Keyboard::TakeConsoleSwitch(uint32_t* console)
{
    if (m_console_switch == NoConsole)
    {
        return false;
    }
    *console = m_console_switch;
    m_console_switch = NoConsole;
    return true;
}

} // namespace FeatherOS::Input
