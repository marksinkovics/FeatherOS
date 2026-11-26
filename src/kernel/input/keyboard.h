#ifndef FEATHEROS_INPUT_KEYBOARD_H
#define FEATHEROS_INPUT_KEYBOARD_H

#include <stddef.h>
#include <stdint.h>

namespace FeatherOS::Input
{

// Key codes as virtio-input reports them (Linux evdev, which USB HID keyboards map to as well).
// PS/2 scancode set 1 uses the same numbers for the main keys and the keypad; its E0-prefixed keys
// map to the codes from KeypadEnter on.
namespace KeyCode
{
constexpr uint16_t Escape = 1;
constexpr uint16_t Backspace = 14;
constexpr uint16_t Tab = 15;
constexpr uint16_t Enter = 28;
constexpr uint16_t LeftControl = 29;
constexpr uint16_t LeftShift = 42;
constexpr uint16_t Z = 44;
constexpr uint16_t C = 46;
constexpr uint16_t RightShift = 54;
constexpr uint16_t LeftAlt = 56;
constexpr uint16_t CapsLock = 58;
constexpr uint16_t F1 = 59;
constexpr uint16_t F4 = 62;
constexpr uint16_t NumLock = 69;
constexpr uint16_t ScrollLock = 70;
constexpr uint16_t KeypadEnter = 96;
constexpr uint16_t RightControl = 97;
constexpr uint16_t RightAlt = 100;
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
constexpr uint16_t LeftMeta = 125;
constexpr uint16_t RightMeta = 126;
} // namespace KeyCode

// The bits of Keyboard::Modifiers, the same as User::FramebufferModifier*.
namespace Modifier
{
constexpr uint8_t Shift = 1;
constexpr uint8_t Control = 2;
constexpr uint8_t Alt = 4;
constexpr uint8_t CapsLock = 8;
constexpr uint8_t Super = 16;
} // namespace Modifier

enum class KeyTransition : uint8_t
{
    None,
    Press,
    Repeat,
    Release,
};

// Turns key presses into the bytes a VT100 terminal sends: ASCII for characters, 1-26 for
// Ctrl+A to Ctrl+Z, '\r' for Enter, 0x7F for Backspace, and escape sequences such as ESC [ A for
// the cursor keys. Alt+F1 to Alt+F4 send nothing; they ask for a virtual console instead. The
// keypad sends digits with Num Lock on and the keys printed below them otherwise. Caps Lock and Num
// Lock are shared by every keyboard (see Input::CapsLock); the modifiers are the keyboard's own.
class Keyboard
{
public:
    static constexpr size_t MaximumKeyBytes = 4;
    static constexpr uint32_t TrackedKeys = 256;

    // Returns how many bytes the key produced: none for releases, modifiers and unmapped keys.
    // A held key may report repeated presses.
    size_t Press(uint16_t code, bool pressed, char (&bytes)[MaximumKeyBytes]);
    // Press for a key not held, Repeat for one held, Release for one held; None for a release of a
    // key not held and for codes from TrackedKeys on. Call it before Press for the same event.
    KeyTransition Track(uint16_t code, bool pressed);
    // Left and right merged, Caps Lock as the lock state, after the events so far.
    uint8_t Modifiers() const;
    // After a Press of Alt+F1 to Alt+F4: true once, with console 0 to 3.
    bool TakeConsoleSwitch(uint32_t* console);

private:
    static constexpr uint32_t NoConsole = UINT32_MAX;

    bool m_left_shift = false;
    bool m_right_shift = false;
    bool m_left_control = false;
    bool m_right_control = false;
    bool m_left_alt = false;
    bool m_right_alt = false;
    bool m_left_meta = false;
    bool m_right_meta = false;
    // Held keys repeat their presses, and a lock key toggles only on the first.
    bool m_caps_lock_held = false;
    bool m_num_lock_held = false;
    uint32_t m_console_switch = NoConsole;
    uint64_t m_held[TrackedKeys / 64] = {};

    char Character(uint16_t code) const;
};

} // namespace FeatherOS::Input

#endif
