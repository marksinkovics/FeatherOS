#include <catch2/catch_test_macros.hpp>

#include "kernel/input/keyboard.h"
#include "libc/include/featheros/framebuffer.h"
#include "user/key_decoder.h"
#include "user/syscall_abi.h"

#include <string>
#include <tuple>
#include <vector>

namespace
{

using FeatherOS::Input::Keyboard;
using FeatherOS::Input::KeyTransition;
namespace KeyCode = FeatherOS::Input::KeyCode;

std::string Press(Keyboard& keyboard, uint16_t code)
{
    char bytes[Keyboard::MaximumKeyBytes];
    std::string text(bytes, keyboard.Press(code, true, bytes));
    char ignored[Keyboard::MaximumKeyBytes];
    REQUIRE(keyboard.Press(code, false, ignored) == 0);
    return text;
}

void Hold(Keyboard& keyboard, uint16_t code, bool pressed)
{
    char bytes[Keyboard::MaximumKeyBytes];
    REQUIRE(keyboard.Press(code, pressed, bytes) == 0);
}

using FeatherOS::User::Key;
using FeatherOS::User::KeyDecoder;
using FeatherOS::User::KeyPress;

std::vector<KeyPress> Decode(KeyDecoder& decoder, const std::string& bytes)
{
    std::vector<KeyPress> keys;
    KeyPress presses[KeyDecoder::MaximumPresses];
    for (const char byte : bytes)
    {
        const size_t found = decoder.Feed(byte, presses);
        keys.insert(keys.end(), presses, presses + found);
    }
    return keys;
}

} // namespace

TEST_CASE("keys produce the bytes a VT100 terminal sends")
{
    Keyboard keyboard;
    REQUIRE(Press(keyboard, 30) == "a");
    REQUIRE(Press(keyboard, 2) == "1");
    REQUIRE(Press(keyboard, 43) == "\\");
    REQUIRE(Press(keyboard, 57) == " ");
    REQUIRE(Press(keyboard, KeyCode::Enter) == "\r");
    REQUIRE(Press(keyboard, KeyCode::KeypadEnter) == "\r");
    REQUIRE(Press(keyboard, KeyCode::Backspace) == "\x7f");
    REQUIRE(Press(keyboard, KeyCode::Tab) == "\t");
    REQUIRE(Press(keyboard, KeyCode::Escape) == "\x1b");
    REQUIRE(Press(keyboard, KeyCode::Up) == "\x1b[A");
    REQUIRE(Press(keyboard, KeyCode::Down) == "\x1b[B");
    REQUIRE(Press(keyboard, KeyCode::Right) == "\x1b[C");
    REQUIRE(Press(keyboard, KeyCode::Left) == "\x1b[D");
    REQUIRE(Press(keyboard, KeyCode::Home) == "\x1b[H");
    REQUIRE(Press(keyboard, KeyCode::End) == "\x1b[F");
    REQUIRE(Press(keyboard, KeyCode::Insert) == "\x1b[2~");
    REQUIRE(Press(keyboard, KeyCode::Delete) == "\x1b[3~");
    REQUIRE(Press(keyboard, KeyCode::PageUp) == "\x1b[5~");
    REQUIRE(Press(keyboard, KeyCode::PageDown) == "\x1b[6~");
    // F1 and unassigned codes produce nothing.
    REQUIRE(Press(keyboard, 59).empty());
    REQUIRE(Press(keyboard, 200).empty());
}

TEST_CASE("Shift, Caps Lock and Ctrl change what a key sends")
{
    Keyboard keyboard;
    Hold(keyboard, KeyCode::LeftShift, true);
    REQUIRE(Press(keyboard, 30) == "A");
    REQUIRE(Press(keyboard, 2) == "!");
    REQUIRE(Press(keyboard, 40) == "\"");
    REQUIRE(Press(keyboard, KeyCode::Tab) == "\x1b[Z");
    // Either Shift key holds it.
    Hold(keyboard, KeyCode::RightShift, true);
    Hold(keyboard, KeyCode::LeftShift, false);
    REQUIRE(Press(keyboard, 30) == "A");
    Hold(keyboard, KeyCode::RightShift, false);
    REQUIRE(Press(keyboard, 30) == "a");

    // Caps Lock toggles on its first press only; a held key repeats its press.
    Hold(keyboard, KeyCode::CapsLock, true);
    Hold(keyboard, KeyCode::CapsLock, true);
    Hold(keyboard, KeyCode::CapsLock, false);
    REQUIRE(Press(keyboard, 30) == "A");
    REQUIRE(Press(keyboard, 2) == "1");
    Hold(keyboard, KeyCode::LeftShift, true);
    REQUIRE(Press(keyboard, 30) == "a");
    Hold(keyboard, KeyCode::LeftShift, false);
    Hold(keyboard, KeyCode::CapsLock, true);
    Hold(keyboard, KeyCode::CapsLock, false);
    REQUIRE(Press(keyboard, 30) == "a");

    Hold(keyboard, KeyCode::LeftControl, true);
    REQUIRE(Press(keyboard, 30) == "\x01");
    REQUIRE(Press(keyboard, 46) == "\x03");
    REQUIRE(Press(keyboard, 26) == "\x1b");
    REQUIRE(Press(keyboard, 2).empty());
    REQUIRE(Press(keyboard, KeyCode::Up) == "\x1b[A");
    Hold(keyboard, KeyCode::LeftControl, false);
    Hold(keyboard, KeyCode::RightControl, true);
    REQUIRE(Press(keyboard, 44) == "\x1a");
    Hold(keyboard, KeyCode::RightControl, false);
    REQUIRE(Press(keyboard, 44) == "z");
}

TEST_CASE("the key decoder names what each key sends")
{
    KeyDecoder decoder;
    const std::vector<KeyPress> keys =
        Decode(decoder, "x \r\n\t\x7f\b\x01\x1a\x1c\x1b[A\x1b[B\x1b[C\x1b[D\x1b[H\x1b[F\x1bOA\x1bOH"
                        "\x1b[1~\x1b[7~\x1b[4~\x1b[8~\x1b[2~\x1b[3~\x1b[5~\x1b[6~\x1b[Z\x1b[1;5C");
    const std::vector<Key> expected = {
        Key::Character, Key::Character, Key::Enter,  Key::Enter,    Key::Tab,      Key::Backspace,
        Key::Backspace, Key::Control,   Key::Control, Key::Control, Key::Up,       Key::Down,
        Key::Right,     Key::Left,      Key::Home,    Key::End,     Key::Up,       Key::Home,
        Key::Home,      Key::Home,      Key::End,     Key::End,     Key::Insert,   Key::Delete,
        Key::PageUp,    Key::PageDown,  Key::BackTab, Key::Right};
    REQUIRE(keys.size() == expected.size());
    for (size_t index = 0; index < keys.size(); ++index)
    {
        INFO("key " << index);
        REQUIRE(keys[index].key == expected[index]);
    }
    REQUIRE(keys[0].character == 'x');
    REQUIRE(keys[1].character == ' ');
    REQUIRE(keys[7].character == 'a');
    REQUIRE(keys[8].character == 'z');
    REQUIRE(keys[9].character == '\\');
    REQUIRE_FALSE(decoder.Pending());
}

TEST_CASE("the key decoder resolves Escape, split sequences and unknown ones")
{
    KeyDecoder decoder;
    KeyPress presses[KeyDecoder::MaximumPresses];
    // A sequence split across reads still decodes.
    REQUIRE(Decode(decoder, "\x1b").empty());
    REQUIRE(decoder.Pending());
    REQUIRE(Decode(decoder, "[").empty());
    std::vector<KeyPress> keys = Decode(decoder, "A");
    REQUIRE(keys.size() == 1);
    REQUIRE(keys[0].key == Key::Up);

    // A lone Escape stays pending until the caller flushes it after a pause.
    REQUIRE(Decode(decoder, "\x1b").empty());
    REQUIRE(decoder.Flush(presses) == 1);
    REQUIRE(presses[0].key == Key::Escape);
    REQUIRE(decoder.Flush(presses) == 0);

    // Escape followed by a byte that starts no sequence gives both keys.
    keys = Decode(decoder, "\x1bq\x1b\x1b[B");
    REQUIRE(keys.size() == 4);
    REQUIRE(keys[0].key == Key::Escape);
    REQUIRE(keys[1].key == Key::Character);
    REQUIRE(keys[1].character == 'q');
    REQUIRE(keys[2].key == Key::Escape);
    REQUIRE(keys[3].key == Key::Down);
    REQUIRE_FALSE(decoder.Pending());

    keys = Decode(decoder, "\x1b[9~\x1b[X\x1bOx\x1b[123456789A\x1b[\x01");
    REQUIRE(keys.size() == 5);
    for (const KeyPress& press : keys)
    {
        REQUIRE(press.key == Key::Unknown);
    }
    REQUIRE(Decode(decoder, "\x1b[").empty());
    REQUIRE(decoder.Flush(presses) == 1);
    REQUIRE(presses[0].key == Key::Unknown);
}

TEST_CASE("Alt+F1 to Alt+F4 ask for a console instead of sending bytes")
{
    Keyboard keyboard;
    uint32_t console = 9;
    REQUIRE(Press(keyboard, KeyCode::F1 + 1).empty());
    REQUIRE_FALSE(keyboard.TakeConsoleSwitch(&console));

    Hold(keyboard, KeyCode::LeftAlt, true);
    REQUIRE(Press(keyboard, KeyCode::F1 + 1).empty());
    REQUIRE(keyboard.TakeConsoleSwitch(&console));
    REQUIRE(console == 1);
    REQUIRE_FALSE(keyboard.TakeConsoleSwitch(&console));
    Hold(keyboard, KeyCode::LeftAlt, false);

    Hold(keyboard, KeyCode::RightAlt, true);
    Hold(keyboard, KeyCode::LeftControl, true);
    REQUIRE(Press(keyboard, KeyCode::F4).empty());
    REQUIRE(keyboard.TakeConsoleSwitch(&console));
    REQUIRE(console == 3);
    Hold(keyboard, KeyCode::LeftControl, false);
    REQUIRE(Press(keyboard, 30) == "a");
    REQUIRE_FALSE(keyboard.TakeConsoleSwitch(&console));
    Hold(keyboard, KeyCode::RightAlt, false);

    REQUIRE(Press(keyboard, KeyCode::F1).empty());
    REQUIRE_FALSE(keyboard.TakeConsoleSwitch(&console));
}

TEST_CASE("Track tells a first press, a repeat and a release, and ignores a stray release")
{
    Keyboard keyboard;
    REQUIRE(keyboard.Track(30, true) == KeyTransition::Press);
    REQUIRE(keyboard.Track(30, true) == KeyTransition::Repeat);
    REQUIRE(keyboard.Track(30, true) == KeyTransition::Repeat);
    REQUIRE(keyboard.Track(31, true) == KeyTransition::Press);
    REQUIRE(keyboard.Track(30, false) == KeyTransition::Release);
    REQUIRE(keyboard.Track(30, false) == KeyTransition::None);
    REQUIRE(keyboard.Track(30, true) == KeyTransition::Press);
    REQUIRE(keyboard.Track(31, false) == KeyTransition::Release);
    REQUIRE(keyboard.Track(32, false) == KeyTransition::None);
    for (const uint16_t code : {256, 300, 1000, 65535})
    {
        REQUIRE(keyboard.Track(code, true) == KeyTransition::None);
        REQUIRE(keyboard.Track(code, true) == KeyTransition::None);
        REQUIRE(keyboard.Track(code, false) == KeyTransition::None);
    }
    for (const uint16_t code : {0, 63, 64, 127, 128, 255})
    {
        REQUIRE(keyboard.Track(code, true) == KeyTransition::Press);
        REQUIRE(keyboard.Track(code, false) == KeyTransition::Release);
    }
}

TEST_CASE("Modifiers merge the left and right keys and follow Caps Lock's toggle")
{
    namespace Modifier = FeatherOS::Input::Modifier;
    Keyboard keyboard;
    char bytes[Keyboard::MaximumKeyBytes];
    REQUIRE(keyboard.Modifiers() == 0);
    for (const auto& [left, right, bit] :
         {std::tuple{KeyCode::LeftShift, KeyCode::RightShift, Modifier::Shift},
          std::tuple{KeyCode::LeftControl, KeyCode::RightControl, Modifier::Control},
          std::tuple{KeyCode::LeftAlt, KeyCode::RightAlt, Modifier::Alt}})
    {
        keyboard.Press(left, true, bytes);
        REQUIRE(keyboard.Modifiers() == bit);
        keyboard.Press(right, true, bytes);
        keyboard.Press(left, false, bytes);
        REQUIRE(keyboard.Modifiers() == bit);
        keyboard.Press(right, false, bytes);
        REQUIRE(keyboard.Modifiers() == 0);
    }
    keyboard.Press(KeyCode::LeftShift, true, bytes);
    keyboard.Press(KeyCode::RightControl, true, bytes);
    keyboard.Press(KeyCode::LeftAlt, true, bytes);
    REQUIRE(keyboard.Modifiers() == (Modifier::Shift | Modifier::Control | Modifier::Alt));
    keyboard.Press(KeyCode::LeftShift, false, bytes);
    keyboard.Press(KeyCode::RightControl, false, bytes);
    keyboard.Press(KeyCode::LeftAlt, false, bytes);

    keyboard.Press(KeyCode::CapsLock, true, bytes);
    REQUIRE(keyboard.Modifiers() == Modifier::CapsLock);
    keyboard.Press(KeyCode::CapsLock, true, bytes);
    keyboard.Press(KeyCode::CapsLock, false, bytes);
    REQUIRE(keyboard.Modifiers() == Modifier::CapsLock);
    keyboard.Press(KeyCode::CapsLock, true, bytes);
    keyboard.Press(KeyCode::CapsLock, false, bytes);
    REQUIRE(keyboard.Modifiers() == 0);
}

TEST_CASE("a virtio-input sequence and a PS/2 one give the same transitions")
{
    const auto run = [](const std::vector<bool>& pressed)
    {
        Keyboard keyboard;
        std::vector<KeyTransition> transitions;
        for (const bool down : pressed)
        {
            transitions.push_back(keyboard.Track(KeyCode::Left, down));
        }
        return transitions;
    };
    std::vector<bool> virtio;
    for (const uint32_t value : {1, 2, 2, 0})
    {
        virtio.push_back(value != 0);
    }
    std::vector<bool> ps2;
    for (const uint8_t scan : {0x4B, 0x4B, 0x4B, 0xCB})
    {
        ps2.push_back((scan & 0x80U) == 0);
    }
    const std::vector<KeyTransition> expected = {KeyTransition::Press, KeyTransition::Repeat,
                                                 KeyTransition::Repeat, KeyTransition::Release};
    REQUIRE(run(virtio) == expected);
    REQUIRE(run(ps2) == expected);
}

TEST_CASE("the KEY_ names are Linux's numbers and what the keyboard sends for them")
{
    struct Letter
    {
        uint16_t code;
        char lower;
    };
    const Letter letters[] = {
        {KEY_1, '1'}, {KEY_2, '2'}, {KEY_3, '3'}, {KEY_4, '4'}, {KEY_5, '5'}, {KEY_6, '6'},
        {KEY_7, '7'}, {KEY_8, '8'}, {KEY_9, '9'}, {KEY_0, '0'}, {KEY_MINUS, '-'},
        {KEY_EQUAL, '='}, {KEY_Q, 'q'}, {KEY_W, 'w'}, {KEY_E, 'e'}, {KEY_R, 'r'}, {KEY_T, 't'},
        {KEY_Y, 'y'}, {KEY_U, 'u'}, {KEY_I, 'i'}, {KEY_O, 'o'}, {KEY_P, 'p'},
        {KEY_LEFTBRACE, '['}, {KEY_RIGHTBRACE, ']'}, {KEY_A, 'a'}, {KEY_S, 's'}, {KEY_D, 'd'},
        {KEY_F, 'f'}, {KEY_G, 'g'}, {KEY_H, 'h'}, {KEY_J, 'j'}, {KEY_K, 'k'}, {KEY_L, 'l'},
        {KEY_SEMICOLON, ';'}, {KEY_APOSTROPHE, '\''}, {KEY_GRAVE, '`'}, {KEY_BACKSLASH, '\\'},
        {KEY_Z, 'z'}, {KEY_X, 'x'}, {KEY_C, 'c'}, {KEY_V, 'v'}, {KEY_B, 'b'}, {KEY_N, 'n'},
        {KEY_M, 'm'}, {KEY_COMMA, ','}, {KEY_DOT, '.'}, {KEY_SLASH, '/'}, {KEY_SPACE, ' '},
    };
    Keyboard keyboard;
    for (const Letter& letter : letters)
    {
        INFO("code " << letter.code);
        REQUIRE(Press(keyboard, letter.code) == std::string(1, letter.lower));
    }
    REQUIRE(KEY_ESC == KeyCode::Escape);
    REQUIRE(KEY_BACKSPACE == KeyCode::Backspace);
    REQUIRE(KEY_TAB == KeyCode::Tab);
    REQUIRE(KEY_ENTER == KeyCode::Enter);
    REQUIRE(KEY_LEFTCTRL == KeyCode::LeftControl);
    REQUIRE(KEY_LEFTSHIFT == KeyCode::LeftShift);
    REQUIRE(KEY_Z == KeyCode::Z);
    REQUIRE(KEY_C == KeyCode::C);
    REQUIRE(KEY_RIGHTSHIFT == KeyCode::RightShift);
    REQUIRE(KEY_LEFTALT == KeyCode::LeftAlt);
    REQUIRE(KEY_CAPSLOCK == KeyCode::CapsLock);
    REQUIRE(KEY_F1 == KeyCode::F1);
    REQUIRE(KEY_F4 == KeyCode::F4);
    REQUIRE(KEY_KPENTER == KeyCode::KeypadEnter);
    REQUIRE(KEY_RIGHTCTRL == KeyCode::RightControl);
    REQUIRE(KEY_RIGHTALT == KeyCode::RightAlt);
    REQUIRE(KEY_HOME == KeyCode::Home);
    REQUIRE(KEY_UP == KeyCode::Up);
    REQUIRE(KEY_PAGEUP == KeyCode::PageUp);
    REQUIRE(KEY_LEFT == KeyCode::Left);
    REQUIRE(KEY_RIGHT == KeyCode::Right);
    REQUIRE(KEY_END == KeyCode::End);
    REQUIRE(KEY_DOWN == KeyCode::Down);
    REQUIRE(KEY_PAGEDOWN == KeyCode::PageDown);
    REQUIRE(KEY_INSERT == KeyCode::Insert);
    REQUIRE(KEY_DELETE == KeyCode::Delete);

    const uint16_t function_keys[] = {KEY_F1, KEY_F2, KEY_F3, KEY_F4, KEY_F5,
                                      KEY_F6, KEY_F7, KEY_F8, KEY_F9, KEY_F10};
    for (uint16_t index = 0; index < 10; ++index)
    {
        REQUIRE(function_keys[index] == 59 + index);
    }
    const uint16_t keypad[] = {KEY_KP7, KEY_KP8, KEY_KP9, KEY_KPMINUS, KEY_KP4,  KEY_KP5, KEY_KP6,
                               KEY_KPPLUS, KEY_KP1, KEY_KP2, KEY_KP3, KEY_KP0, KEY_KPDOT};
    for (uint16_t index = 0; index < 13; ++index)
    {
        REQUIRE(keypad[index] == 71 + index);
    }
    REQUIRE(KEY_KPASTERISK == 55);
    REQUIRE(KEY_NUMLOCK == 69);
    REQUIRE(KEY_SCROLLLOCK == 70);
    REQUIRE(KEY_F11 == 87);
    REQUIRE(KEY_F12 == 88);
}

TEST_CASE("the libc framebuffer event and constants match the kernel's")
{
    namespace User = FeatherOS::User;
    REQUIRE(sizeof(fb_event) == 8);
    REQUIRE(sizeof(fb_event) == sizeof(User::FramebufferEvent));
    REQUIRE(offsetof(fb_event, code) == 0);
    REQUIRE(offsetof(fb_event, kind) == 2);
    REQUIRE(offsetof(fb_event, modifiers) == 3);
    REQUIRE(offsetof(fb_event, time_ms) == 4);
    REQUIRE(FB_EVENT_RELEASE == User::FramebufferEventRelease);
    REQUIRE(FB_EVENT_PRESS == User::FramebufferEventPress);
    REQUIRE(FB_EVENT_REPEAT == User::FramebufferEventRepeat);
    REQUIRE(FB_EVENT_SHOWN == User::FramebufferEventShown);
    REQUIRE(FB_EVENT_HIDDEN == User::FramebufferEventHidden);
    REQUIRE(FB_MOD_SHIFT == FeatherOS::Input::Modifier::Shift);
    REQUIRE(FB_MOD_CONTROL == FeatherOS::Input::Modifier::Control);
    REQUIRE(FB_MOD_ALT == FeatherOS::Input::Modifier::Alt);
    REQUIRE(FB_MOD_CAPS_LOCK == FeatherOS::Input::Modifier::CapsLock);
    REQUIRE(FB_EVENT_CAPACITY == User::FramebufferEventCapacity);
}
