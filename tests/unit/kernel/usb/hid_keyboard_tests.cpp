#include <catch2/catch_test_macros.hpp>

#include "kernel/input/input.h"
#include "kernel/input/keyboard.h"
#include "kernel/usb/hid/keyboard.h"

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace
{

using namespace FeatherOS::Usb::Hid;
namespace KeyCode = FeatherOS::Input::KeyCode;

KeySet Keys(uint8_t modifiers, std::initializer_list<uint16_t> usages)
{
    KeySet set;
    set.modifiers = modifiers;
    for (const uint16_t usage : usages)
    {
        set.keys[set.count++] = usage;
    }
    return set;
}

using Events = std::vector<std::pair<uint16_t, bool>>;

Events Changes(const KeySet& before, const KeySet& after)
{
    KeyEvent out[MaxKeyEvents];
    const size_t count = Diff(before, after, out, MaxKeyEvents);
    Events events;
    for (size_t i = 0; i < count; ++i)
    {
        events.emplace_back(out[i].code, out[i].pressed);
    }
    return events;
}

// The codes of a keyboard written down by rows of the US layout, independent of the table.
uint16_t RowCode(char letter)
{
    static const std::string top = "qwertyuiop";
    static const std::string home = "asdfghjkl";
    static const std::string bottom = "zxcvbnm";
    if (const size_t i = top.find(letter); i != std::string::npos)
    {
        return static_cast<uint16_t>(16 + i);
    }
    if (const size_t i = home.find(letter); i != std::string::npos)
    {
        return static_cast<uint16_t>(30 + i);
    }
    return static_cast<uint16_t>(44 + bottom.find(letter));
}

std::string Type(FeatherOS::Input::Keyboard& keyboard, uint16_t usage)
{
    const uint16_t code = UsageToKeyCode(usage);
    REQUIRE(code != 0);
    char bytes[FeatherOS::Input::Keyboard::MaximumKeyBytes];
    std::string text(bytes, keyboard.Press(code, true, bytes));
    char ignored[FeatherOS::Input::Keyboard::MaximumKeyBytes];
    keyboard.Press(code, false, ignored);
    return text;
}

void Modifier(FeatherOS::Input::Keyboard& keyboard, uint16_t usage, bool pressed)
{
    char bytes[FeatherOS::Input::Keyboard::MaximumKeyBytes];
    keyboard.Press(UsageToKeyCode(usage), pressed, bytes);
}

} // namespace

TEST_CASE("hid T21: a press, a release and a held key", "[usb][hid][keyboard]")
{
    REQUIRE(Changes(Keys(0, {}), Keys(0, {0x04})) == Events{{30, true}});
    REQUIRE(Changes(Keys(0, {0x04}), Keys(0, {0x04})).empty());
    REQUIRE(Changes(Keys(0, {0x04}), Keys(0, {})) == Events{{30, false}});
    REQUIRE(Changes(Keys(0, {0x04}), Keys(0, {0x04, 0x05})) == Events{{48, true}});
    REQUIRE(Changes(Keys(0, {0x04, 0x05}), Keys(0, {0x05})) == Events{{30, false}});
}

TEST_CASE("hid T21: releases come before presses and modifiers surround the keys",
          "[usb][hid][keyboard]")
{
    // Key 4 and left Control go, right Shift and key 5 arrive: the key is released first, the
    // modifier pressed before the key it modifies.
    REQUIRE(Changes(Keys(0x01, {0x04}), Keys(0x20, {0x05})) ==
            Events{{30, false}, {29, false}, {54, true}, {48, true}});
    // Shift and a key in one report.
    REQUIRE(Changes(Keys(0, {}), Keys(0x02, {0x04})) == Events{{42, true}, {30, true}});
    REQUIRE(Changes(Keys(0x02, {0x04}), Keys(0, {})) == Events{{30, false}, {42, false}});
}

TEST_CASE("hid T21: six keys at once and every modifier", "[usb][hid][keyboard]")
{
    const Events presses =
        Changes(Keys(0, {}), Keys(0, {0x04, 0x05, 0x06, 0x07, 0x08, 0x09}));
    REQUIRE(presses ==
            Events{{30, true}, {48, true}, {46, true}, {32, true}, {18, true}, {33, true}});
    const Events modifiers = Changes(Keys(0, {}), Keys(0xFF, {}));
    REQUIRE(modifiers == Events{{29, true}, {42, true}, {56, true}, {125, true},
                                {97, true}, {54, true}, {100, true}, {126, true}});
}

TEST_CASE("hid T21: a rollover report changes nothing", "[usb][hid][keyboard]")
{
    KeySet rollover;
    rollover.rollover = true;
    REQUIRE(Changes(Keys(0x02, {0x04}), rollover).empty());

    KeySet down = Keys(0x02, {0x04});
    KeyEvent out[MaxKeyEvents];
    REQUIRE(Advance(&down, rollover, out, MaxKeyEvents) == 0);
    REQUIRE(down.modifiers == 0x02);
    REQUIRE(down.count == 1);
    // The keys that follow compare with what was down before the rollover.
    REQUIRE(Advance(&down, Keys(0x02, {0x04, 0x05}), out, MaxKeyEvents) == 1);
    REQUIRE(out[0].code == 48);
    REQUIRE(out[0].pressed);
}

TEST_CASE("hid T21: usages without a key give no event, press or release", "[usb][hid][keyboard]")
{
    REQUIRE(Changes(Keys(0, {}), Keys(0, {0x65, 0x04})) == Events{{30, true}});
    REQUIRE(Changes(Keys(0, {0x65, 0x04}), Keys(0, {})) == Events{{30, false}});
}

TEST_CASE("hid T21: releasing everything that is down, as on a detach", "[usb][hid][keyboard]")
{
    KeySet down = Keys(0x03, {0x04, 0x05, 0x65});
    KeyEvent out[MaxKeyEvents];
    const size_t count = ReleaseAll(&down, out, MaxKeyEvents);
    REQUIRE(count == 4);
    std::set<uint16_t> released;
    for (size_t i = 0; i < count; ++i)
    {
        REQUIRE_FALSE(out[i].pressed);
        released.insert(out[i].code);
    }
    REQUIRE(released == std::set<uint16_t>{29, 42, 30, 48});
    REQUIRE(down.count == 0);
    REQUIRE(down.modifiers == 0);
    REQUIRE(ReleaseAll(&down, out, MaxKeyEvents) == 0);
}

TEST_CASE("hid T21: every press has exactly one release over random reports",
          "[usb][hid][keyboard]")
{
    uint32_t seed = 12345;
    const auto next = [&seed]()
    {
        seed = seed * 1664525U + 1013904223U;
        return seed >> 8;
    };
    for (int run = 0; run < 200; ++run)
    {
        KeySet down;
        // Usages 0x31 and 0x32 share a code, so a code can be down twice.
        std::map<uint16_t, int> pressed;
        const auto apply = [&](const KeyEvent* events, size_t count)
        {
            for (size_t i = 0; i < count; ++i)
            {
                if (events[i].pressed)
                {
                    ++pressed[events[i].code];
                }
                else
                {
                    REQUIRE(pressed[events[i].code] > 0);
                    if (--pressed[events[i].code] == 0)
                    {
                        pressed.erase(events[i].code);
                    }
                }
            }
        };
        KeyEvent out[MaxKeyEvents];
        for (int step = 0; step < 40; ++step)
        {
            KeySet report;
            report.modifiers = static_cast<uint8_t>(next());
            const uint32_t keys = next() % 8;
            for (uint32_t k = 0; k < keys; ++k)
            {
                const auto usage = static_cast<uint16_t>(next() % 0x70);
                bool duplicate = usage == 0;
                for (size_t i = 0; i < report.count; ++i)
                {
                    duplicate = duplicate || report.keys[i] == usage;
                }
                if (!duplicate)
                {
                    report.keys[report.count++] = usage;
                }
            }
            report.rollover = next() % 10 == 0;
            apply(out, Advance(&down, report, out, MaxKeyEvents));
        }
        apply(out, ReleaseAll(&down, out, MaxKeyEvents));
        REQUIRE(pressed.empty());
    }
}

TEST_CASE("hid T22: the usage table matches the Usage Tables and evdev", "[usb][hid][keyboard]")
{
    std::vector<std::pair<uint16_t, uint16_t>> expected;
    for (char letter = 'a'; letter <= 'z'; ++letter)
    {
        expected.emplace_back(4 + (letter - 'a'), RowCode(letter));
    }
    const std::vector<std::pair<uint16_t, uint16_t>> fixed = {
        {0x1E, 2},   {0x1F, 3},   {0x20, 4},   {0x21, 5},   {0x22, 6},   {0x23, 7},   {0x24, 8},
        {0x25, 9},   {0x26, 10},  {0x27, 11},  {0x28, 28},  {0x29, 1},   {0x2A, 14},  {0x2B, 15},
        {0x2C, 57},  {0x2D, 12},  {0x2E, 13},  {0x2F, 26},  {0x30, 27},  {0x31, 43},  {0x32, 43},
        {0x33, 39},  {0x34, 40},  {0x35, 41},  {0x36, 51},  {0x37, 52},  {0x38, 53},  {0x39, 58},
        {0x3A, 59},  {0x3B, 60},  {0x3C, 61},  {0x3D, 62},  {0x3E, 63},  {0x3F, 64},  {0x40, 65},
        {0x41, 66},  {0x42, 67},  {0x43, 68},  {0x44, 87},  {0x45, 88},  {0x46, 99},  {0x47, 70},
        {0x48, 119}, {0x49, 110}, {0x4A, 102}, {0x4B, 104}, {0x4C, 111}, {0x4D, 107}, {0x4E, 109},
        {0x4F, 106}, {0x50, 105}, {0x51, 108}, {0x52, 103}, {0x53, 69},  {0x54, 98},  {0x55, 55},
        {0x56, 74},  {0x57, 78},  {0x58, 96},  {0x59, 79},  {0x5A, 80},  {0x5B, 81},  {0x5C, 75},
        {0x5D, 76},  {0x5E, 77},  {0x5F, 71},  {0x60, 72},  {0x61, 73},  {0x62, 82},  {0x63, 83},
        {0x64, 86},  {0xE0, 29},  {0xE1, 42},  {0xE2, 56},  {0xE3, 125}, {0xE4, 97},  {0xE5, 54},
        {0xE6, 100}, {0xE7, 126}};
    expected.insert(expected.end(), fixed.begin(), fixed.end());
    std::set<uint16_t> mapped;
    for (const auto& [usage, code] : expected)
    {
        REQUIRE(UsageToKeyCode(usage) == code);
        mapped.insert(usage);
    }
    for (uint32_t usage = 0; usage <= 0xFFFF; ++usage)
    {
        if (mapped.count(static_cast<uint16_t>(usage)) == 0)
        {
            REQUIRE(UsageToKeyCode(static_cast<uint16_t>(usage)) == 0);
        }
    }
}

TEST_CASE("hid T22: usages reach the terminal as bytes through Input::Keyboard",
          "[usb][hid][keyboard]")
{
    FeatherOS::Input::SetLockState(false, false);
    FeatherOS::Input::Keyboard keyboard;
    for (char letter = 'a'; letter <= 'z'; ++letter)
    {
        REQUIRE(Type(keyboard, static_cast<uint16_t>(4 + (letter - 'a'))) == std::string(1, letter));
    }
    REQUIRE(Type(keyboard, 0x1E) == "1");
    REQUIRE(Type(keyboard, 0x27) == "0");
    REQUIRE(Type(keyboard, 0x28) == "\r");
    REQUIRE(Type(keyboard, 0x2C) == " ");

    Modifier(keyboard, 0xE1, true);
    REQUIRE(Type(keyboard, 0x04) == "A");
    Modifier(keyboard, 0xE1, false);
    Modifier(keyboard, 0xE0, true);
    REQUIRE(Type(keyboard, 0x06) == "\x03");
    Modifier(keyboard, 0xE0, false);

    REQUIRE(Type(keyboard, 0x52) == "\x1b[A");
    REQUIRE(Type(keyboard, 0x51) == "\x1b[B");
    REQUIRE(Type(keyboard, 0x4F) == "\x1b[C");
    REQUIRE(Type(keyboard, 0x50) == "\x1b[D");
    REQUIRE(Type(keyboard, 0x4A) == "\x1b[H");
    REQUIRE(Type(keyboard, 0x4D) == "\x1b[F");
    REQUIRE(Type(keyboard, 0x4C) == "\x1b[3~");
}

TEST_CASE("hid T22: the keypad follows Num Lock", "[usb][hid][keyboard]")
{
    FeatherOS::Input::Keyboard keyboard;
    struct Key
    {
        uint16_t usage;
        const char* num_lock_off;
        const char* num_lock_on;
    };
    const Key keys[] = {
        {0x59, "\x1b[F", "1"},  {0x5A, "\x1b[B", "2"},  {0x5B, "\x1b[6~", "3"},
        {0x5C, "\x1b[D", "4"},  {0x5D, "", "5"},        {0x5E, "\x1b[C", "6"},
        {0x5F, "\x1b[H", "7"},  {0x60, "\x1b[A", "8"},  {0x61, "\x1b[5~", "9"},
        {0x62, "\x1b[2~", "0"}, {0x63, "\x1b[3~", "."}, {0x54, "/", "/"},
        {0x55, "*", "*"},       {0x56, "-", "-"},       {0x57, "+", "+"},
        {0x58, "\r", "\r"},
    };
    for (const bool num_lock : {false, true})
    {
        FeatherOS::Input::SetLockState(false, num_lock);
        for (const Key& key : keys)
        {
            REQUIRE(Type(keyboard, key.usage) == (num_lock ? key.num_lock_on : key.num_lock_off));
        }
    }
    FeatherOS::Input::SetLockState(false, false);
}

TEST_CASE("hid T23: the repeater waits 500 ms, then repeats every 33 ms", "[usb][hid][keyboard]")
{
    Repeater repeater(1000);
    uint16_t code = 0;
    REQUIRE(repeater.Deadline() == UINT64_MAX);
    REQUIRE_FALSE(repeater.Due(100000, &code));

    repeater.Press(30, 100);
    REQUIRE(repeater.Deadline() == 600);
    REQUIRE_FALSE(repeater.Due(599, &code));
    REQUIRE(repeater.Due(600, &code));
    REQUIRE(code == 30);
    REQUIRE(repeater.Deadline() == 633);
    REQUIRE_FALSE(repeater.Due(632, &code));
    REQUIRE(repeater.Due(633, &code));
    REQUIRE(repeater.Due(666, &code));
    REQUIRE(repeater.Deadline() == 699);

    // A late poll gives one repeat, not a burst.
    REQUIRE(repeater.Due(2000, &code));
    REQUIRE_FALSE(repeater.Due(2000, &code));
    REQUIRE(repeater.Deadline() == 2033);
}

TEST_CASE("hid T23: modifiers and locks never repeat and leave the repeating key alone",
          "[usb][hid][keyboard]")
{
    const uint16_t never[] = {KeyCode::LeftControl, KeyCode::LeftShift, KeyCode::RightShift,
                              KeyCode::LeftAlt,     KeyCode::RightControl, KeyCode::RightAlt,
                              KeyCode::LeftMeta,    KeyCode::RightMeta,    KeyCode::CapsLock,
                              KeyCode::NumLock,     KeyCode::ScrollLock,   0};
    uint16_t code = 0;
    for (const uint16_t key : never)
    {
        Repeater repeater(1000);
        repeater.Press(key, 0);
        REQUIRE(repeater.Deadline() == UINT64_MAX);
        REQUIRE_FALSE(repeater.Due(10000, &code));
    }

    Repeater repeater(1000);
    repeater.Press(30, 0);
    repeater.Press(KeyCode::LeftShift, 100);
    repeater.Press(KeyCode::CapsLock, 100);
    REQUIRE(repeater.Deadline() == 500);
    REQUIRE(repeater.Due(500, &code));
    REQUIRE(code == 30);
}

TEST_CASE("hid T23: a new press replaces the key; a release, a new press or a stop ends it",
          "[usb][hid][keyboard]")
{
    Repeater repeater(1000);
    uint16_t code = 0;
    repeater.Press(30, 0);
    REQUIRE(repeater.Due(500, &code));
    repeater.Press(48, 700);
    REQUIRE(repeater.Deadline() == 1200);
    REQUIRE_FALSE(repeater.Due(1199, &code));
    REQUIRE(repeater.Due(1200, &code));
    REQUIRE(code == 48);

    repeater.Release(30);
    REQUIRE(repeater.Deadline() == 1233);
    REQUIRE(repeater.Due(1233, &code));
    REQUIRE(code == 48);
    repeater.Release(48);
    REQUIRE(repeater.Deadline() == UINT64_MAX);
    REQUIRE_FALSE(repeater.Due(100000, &code));

    repeater.Press(31, 0);
    repeater.Stop();
    REQUIRE_FALSE(repeater.Due(100000, &code));
    repeater.Press(31, 5000);
    REQUIRE(repeater.Deadline() == 5500);
}

TEST_CASE("hid T23: the delay and rate follow the tick rate", "[usb][hid][keyboard]")
{
    Repeater repeater(100);
    uint16_t code = 0;
    repeater.Press(30, 10);
    REQUIRE(repeater.Deadline() == 60);
    REQUIRE(repeater.Due(60, &code));
    REQUIRE(repeater.Deadline() == 64);
}
