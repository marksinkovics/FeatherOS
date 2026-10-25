#include <catch2/catch_test_macros.hpp>

#include "kernel/input/keyboard.h"
#include "user/key_decoder.h"

#include <string>
#include <vector>

namespace
{

using FeatherOS::Input::Keyboard;
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
