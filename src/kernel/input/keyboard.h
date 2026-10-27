#ifndef FEATHEROS_INPUT_KEYBOARD_H
#define FEATHEROS_INPUT_KEYBOARD_H

#include <stddef.h>
#include <stdint.h>

namespace FeatherOS::Input
{

// Key codes as virtio-input reports them. PS/2 scancode set 1 uses the same numbers for the main
// keys; its E0-prefixed keys map to the codes from KeypadEnter on.
namespace KeyCode
{
constexpr uint16_t Escape = 1;
constexpr uint16_t Backspace = 14;
constexpr uint16_t Tab = 15;
constexpr uint16_t Enter = 28;
constexpr uint16_t LeftControl = 29;
constexpr uint16_t LeftShift = 42;
constexpr uint16_t RightShift = 54;
constexpr uint16_t CapsLock = 58;
constexpr uint16_t KeypadEnter = 96;
constexpr uint16_t RightControl = 97;
constexpr uint16_t Home = 102;
constexpr uint16_t Up = 103;
constexpr uint16_t PageUp = 104;
constexpr uint16_t Left = 105;
constexpr uint16_t Right = 106;
constexpr uint16_t End = 107;
constexpr uint16_t Down = 108;
constexpr uint16_t PageDown = 109;
constexpr uint16_t Insert = 110;
constexpr uint16_t Delete = 111;
} // namespace KeyCode

// Turns key presses into the bytes a VT100 terminal sends: ASCII for characters, 1-26 for
// Ctrl+A to Ctrl+Z, '\r' for Enter, 0x7F for Backspace, and escape sequences such as ESC [ A for
// the cursor keys.
class Keyboard
{
public:
    static constexpr size_t MaximumKeyBytes = 4;

    // Returns how many bytes the key produced: none for releases, modifiers and unmapped keys.
    // A held key may report repeated presses.
    size_t Press(uint16_t code, bool pressed, char (&bytes)[MaximumKeyBytes]);

private:
    bool m_left_shift = false;
    bool m_right_shift = false;
    bool m_left_control = false;
    bool m_right_control = false;
    bool m_caps_lock = false;
    // Held keys repeat their presses, and Caps Lock toggles only on the first.
    bool m_caps_lock_held = false;

    char Character(uint16_t code) const;
};

} // namespace FeatherOS::Input

#endif
