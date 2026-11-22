#include <catch2/catch_test_macros.hpp>

#include "kernel/arch/serial.h"
#include "kernel/input/input.h"
#include "kernel/input/keyboard.h"
#include "kernel/terminal/terminal.h"
#include "support/scheduler_stubs.h"
#include "support/terminal_device.h"

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace
{

std::deque<char> serial_input;

// Raw and without echo, so a read returns exactly the queued bytes.
std::string ReadAll(FeatherOS::Terminals::Terminal& terminal)
{
    terminal.SetSettings({0, 0, 0, 0}, 1);
    std::string text;
    char buffer[64];
    for (;;)
    {
        const uint64_t count = terminal.Read(0, buffer, sizeof(buffer));
        REQUIRE_FALSE(FeatherOS::User::IsSyscallError(count));
        if (count == 0)
        {
            return text;
        }
        text.append(buffer, count);
    }
}

} // namespace

namespace FeatherOS::Serial
{

bool ReadCharacter(char* character)
{
    if (character == nullptr || serial_input.empty())
    {
        return false;
    }
    *character = serial_input.front();
    serial_input.pop_front();
    return true;
}

} // namespace FeatherOS::Serial

TEST_CASE("serial input reaches the terminal as typed, with CR LF counted once")
{
    TestScheduler::Reset();
    TestTerminalDevice device;
    FeatherOS::Terminals::Terminal terminal(device.Device());
    FeatherOS::Input::SetTerminal(&terminal);
    serial_input = {'a', '\r', '\n', 'b', '\x7f', '\x1b', '[', 'A', '\n', '\n'};
    FeatherOS::Input::Poll();
    REQUIRE(ReadAll(terminal) == "a\rb\x7f\x1b[A\n\n");

    FeatherOS::Input::SubmitBytes("\x1b[B", 3);
    REQUIRE(ReadAll(terminal) == "\x1b[B");
    FeatherOS::Input::SetTerminal(nullptr);
}

TEST_CASE("a serial Ctrl+C reaches the terminal alone, after the bytes before it")
{
    TestScheduler::Reset();
    TestTerminalDevice device;
    FeatherOS::Terminals::Terminal terminal(device.Device());
    FeatherOS::Input::SetTerminal(&terminal);
    static int interrupts = 0;
    FeatherOS::Terminals::SetProcessHooks({[](uint64_t, int)
                                           {
                                               ++interrupts;
                                               return true;
                                           },
                                           nullptr});

    serial_input = {'a', '\x03', 'b'};
    FeatherOS::Input::Poll();
    REQUIRE(interrupts == 1);
    REQUIRE(ReadAll(terminal) == "ab");

    FeatherOS::Terminals::SetProcessHooks({nullptr, nullptr});
    FeatherOS::Input::SetTerminal(nullptr);
}

TEST_CASE("keys go to the shown console's terminal, serial to the first, Alt+Fn switches")
{
    TestScheduler::Reset();
    TestTerminalDevice device;
    FeatherOS::Terminals::Terminal first(device.Device());
    FeatherOS::Terminals::Terminal second(device.Device());
    FeatherOS::Input::SetTerminal(&first);
    static FeatherOS::Terminals::Terminal* shown = nullptr;
    static FeatherOS::Terminals::Terminal* consoles[2] = {&first, &second};
    shown = &first;
    FeatherOS::Input::SetConsoleSwitch(
        [](uint32_t console)
        {
            if (console < 2)
            {
                shown = consoles[console];
                FeatherOS::Input::SetKeyboardTerminal(shown);
            }
        });

    namespace KeyCode = FeatherOS::Input::KeyCode;
    FeatherOS::Input::Keyboard keyboard;
    const auto tap = [&](uint16_t code)
    {
        FeatherOS::Input::SubmitKey(keyboard, code, true);
        FeatherOS::Input::SubmitKey(keyboard, code, false);
    };
    FeatherOS::Input::SubmitKey(keyboard, KeyCode::LeftAlt, true);
    tap(KeyCode::F1 + 1);
    FeatherOS::Input::SubmitKey(keyboard, KeyCode::LeftAlt, false);
    REQUIRE(shown == &second);
    tap(30);
    serial_input = {'s'};
    FeatherOS::Input::Poll();
    REQUIRE(ReadAll(second) == "a");
    REQUIRE(ReadAll(first) == "s");

    // A console that does not exist leaves the keys where they were.
    FeatherOS::Input::SubmitKey(keyboard, KeyCode::RightAlt, true);
    tap(KeyCode::F4);
    FeatherOS::Input::SubmitKey(keyboard, KeyCode::RightAlt, false);
    tap(48);
    REQUIRE(ReadAll(second) == "b");

    FeatherOS::Input::SetConsoleSwitch(nullptr);
    FeatherOS::Input::SetTerminal(nullptr);
}

TEST_CASE("input without a terminal is dropped")
{
    serial_input = {'x'};
    FeatherOS::Input::Poll();
    FeatherOS::Input::SubmitBytes("y", 1);
    REQUIRE(serial_input.empty());
}

namespace
{

struct HookCall
{
    uint16_t code;
    FeatherOS::Input::KeyTransition transition;
    uint8_t modifiers;
};

std::vector<HookCall> hook_calls;
bool hook_takes = false;

bool RecordKey(const FeatherOS::Input::KeyEvent& event)
{
    hook_calls.push_back({event.code, event.transition, event.modifiers});
    return hook_takes;
}

std::vector<uint8_t> led_changes;

} // namespace

TEST_CASE("a key the graphics hook takes sends no bytes; one it refuses sends today's")
{
    namespace KeyCode = FeatherOS::Input::KeyCode;
    TestScheduler::Reset();
    TestTerminalDevice device;
    FeatherOS::Terminals::Terminal terminal(device.Device());
    FeatherOS::Input::SetTerminal(&terminal);
    FeatherOS::Input::SetGraphicsKeys(RecordKey);
    FeatherOS::Input::Keyboard keyboard;
    hook_calls.clear();

    hook_takes = true;
    FeatherOS::Input::SubmitKey(keyboard, 30, true);
    FeatherOS::Input::SubmitKey(keyboard, 30, true);
    FeatherOS::Input::SubmitKey(keyboard, 30, false);
    REQUIRE(ReadAll(terminal).empty());
    REQUIRE(hook_calls.size() == 3);
    REQUIRE(hook_calls[0].transition == FeatherOS::Input::KeyTransition::Press);
    REQUIRE(hook_calls[1].transition == FeatherOS::Input::KeyTransition::Repeat);
    REQUIRE(hook_calls[2].transition == FeatherOS::Input::KeyTransition::Release);

    hook_takes = false;
    hook_calls.clear();
    FeatherOS::Input::SubmitKey(keyboard, 30, true);
    FeatherOS::Input::SubmitKey(keyboard, 30, false);
    FeatherOS::Input::SubmitKey(keyboard, KeyCode::Up, true);
    FeatherOS::Input::SubmitKey(keyboard, KeyCode::Up, false);
    REQUIRE(ReadAll(terminal) == "a\x1b[A");
    REQUIRE(hook_calls.size() == 4);

    FeatherOS::Input::SetGraphicsKeys(nullptr);
    FeatherOS::Input::SubmitKey(keyboard, 31, true);
    FeatherOS::Input::SubmitKey(keyboard, 31, false);
    REQUIRE(ReadAll(terminal) == "s");
    FeatherOS::Input::SetTerminal(nullptr);
}

TEST_CASE("the hook gets the modifiers after the event, and never an untracked release")
{
    namespace KeyCode = FeatherOS::Input::KeyCode;
    namespace Modifier = FeatherOS::Input::Modifier;
    using FeatherOS::Input::KeyTransition;
    FeatherOS::Input::SetGraphicsKeys(RecordKey);
    FeatherOS::Input::Keyboard keyboard;
    hook_calls.clear();
    hook_takes = true;

    FeatherOS::Input::SubmitKey(keyboard, 30, false);
    FeatherOS::Input::SubmitKey(keyboard, 300, true);
    FeatherOS::Input::SubmitKey(keyboard, 300, false);
    REQUIRE(hook_calls.empty());

    FeatherOS::Input::SubmitKey(keyboard, KeyCode::LeftShift, true);
    FeatherOS::Input::SubmitKey(keyboard, 30, true);
    FeatherOS::Input::SubmitKey(keyboard, 30, false);
    FeatherOS::Input::SubmitKey(keyboard, KeyCode::LeftShift, false);
    REQUIRE(hook_calls.size() == 4);
    REQUIRE(hook_calls[0].code == KeyCode::LeftShift);
    REQUIRE(hook_calls[0].modifiers == Modifier::Shift);
    REQUIRE(hook_calls[1].modifiers == Modifier::Shift);
    REQUIRE(hook_calls[2].modifiers == Modifier::Shift);
    REQUIRE(hook_calls[3].transition == KeyTransition::Release);
    REQUIRE(hook_calls[3].modifiers == 0);
    REQUIRE(FeatherOS::Input::CurrentModifiers() == 0);
    FeatherOS::Input::SetGraphicsKeys(nullptr);
}

TEST_CASE("Alt+Fn switches consoles and its press never reaches the hook")
{
    namespace KeyCode = FeatherOS::Input::KeyCode;
    using FeatherOS::Input::KeyTransition;
    static std::vector<uint32_t> switches;
    static uint8_t modifiers_at_switch = 0;
    switches.clear();
    FeatherOS::Input::SetConsoleSwitch(
        [](uint32_t console)
        {
            switches.push_back(console);
            modifiers_at_switch = FeatherOS::Input::CurrentModifiers();
        });
    FeatherOS::Input::SetGraphicsKeys(RecordKey);
    FeatherOS::Input::Keyboard keyboard;
    hook_calls.clear();
    hook_takes = true;

    FeatherOS::Input::SubmitKey(keyboard, KeyCode::LeftAlt, true);
    FeatherOS::Input::SubmitKey(keyboard, KeyCode::F1 + 1, true);
    FeatherOS::Input::SubmitKey(keyboard, KeyCode::F1 + 1, false);
    FeatherOS::Input::SubmitKey(keyboard, KeyCode::LeftAlt, false);
    REQUIRE(switches == std::vector<uint32_t>{1});
    REQUIRE(modifiers_at_switch == FeatherOS::Input::Modifier::Alt);
    REQUIRE(hook_calls.size() == 3);
    REQUIRE(hook_calls[0].code == KeyCode::LeftAlt);
    REQUIRE(hook_calls[1].code == KeyCode::F1 + 1);
    REQUIRE(hook_calls[1].transition == KeyTransition::Release);
    REQUIRE(hook_calls[2].code == KeyCode::LeftAlt);

    FeatherOS::Input::SetGraphicsKeys(nullptr);
    FeatherOS::Input::SetConsoleSwitch(nullptr);
}

TEST_CASE("IsInterruptKey is a first press of C with Control, whatever else is held")
{
    namespace KeyCode = FeatherOS::Input::KeyCode;
    namespace Modifier = FeatherOS::Input::Modifier;
    using FeatherOS::Input::IsInterruptKey;
    using FeatherOS::Input::KeyTransition;
    REQUIRE(IsInterruptKey({KeyCode::C, KeyTransition::Press, Modifier::Control}));
    REQUIRE(IsInterruptKey(
        {KeyCode::C, KeyTransition::Press, Modifier::Control | Modifier::Shift}));
    REQUIRE(IsInterruptKey(
        {KeyCode::C, KeyTransition::Press, Modifier::Control | Modifier::CapsLock}));
    REQUIRE_FALSE(IsInterruptKey({KeyCode::C, KeyTransition::Repeat, Modifier::Control}));
    REQUIRE_FALSE(IsInterruptKey({KeyCode::C, KeyTransition::Release, Modifier::Control}));
    REQUIRE_FALSE(IsInterruptKey({KeyCode::C, KeyTransition::Press, 0}));
    REQUIRE_FALSE(IsInterruptKey({KeyCode::C, KeyTransition::Press, Modifier::Shift}));
    REQUIRE_FALSE(IsInterruptKey({KeyCode::Z, KeyTransition::Press, Modifier::Control}));

    FeatherOS::Input::Keyboard keyboard;
    for (const uint16_t control : {KeyCode::LeftControl, KeyCode::RightControl})
    {
        static FeatherOS::Input::KeyEvent last;
        FeatherOS::Input::SetGraphicsKeys(
            [](const FeatherOS::Input::KeyEvent& event)
            {
                last = event;
                return true;
            });
        FeatherOS::Input::SubmitKey(keyboard, control, true);
        FeatherOS::Input::SubmitKey(keyboard, KeyCode::C, true);
        REQUIRE(IsInterruptKey(last));
        FeatherOS::Input::SubmitKey(keyboard, KeyCode::C, true);
        REQUIRE_FALSE(IsInterruptKey(last));
        FeatherOS::Input::SubmitKey(keyboard, KeyCode::C, false);
        FeatherOS::Input::SubmitKey(keyboard, control, false);
    }
    FeatherOS::Input::SetGraphicsKeys(nullptr);
}

TEST_CASE("input T24: Caps Lock and Num Lock toggle once per press and are shared")
{
    namespace KeyCode = FeatherOS::Input::KeyCode;
    FeatherOS::Input::SetLockState(false, false);
    led_changes.clear();
    FeatherOS::Input::SetLedCallback([](uint8_t state) { led_changes.push_back(state); });
    REQUIRE(FeatherOS::Input::LedState() == 0);

    FeatherOS::Input::Keyboard first;
    FeatherOS::Input::Keyboard second;
    char bytes[FeatherOS::Input::Keyboard::MaximumKeyBytes];

    // A held key repeats its press, and the release changes nothing.
    first.Press(KeyCode::CapsLock, true, bytes);
    first.Press(KeyCode::CapsLock, true, bytes);
    first.Press(KeyCode::CapsLock, true, bytes);
    first.Press(KeyCode::CapsLock, false, bytes);
    REQUIRE(FeatherOS::Input::CapsLock());
    REQUIRE(FeatherOS::Input::LedState() == FeatherOS::Input::LedCapsLock);
    REQUIRE(led_changes == std::vector<uint8_t>{2});

    // The other keyboard types capitals, and its Num Lock press is the same lock.
    REQUIRE(second.Press(30, true, bytes) == 1);
    REQUIRE(bytes[0] == 'A');
    second.Press(KeyCode::NumLock, true, bytes);
    second.Press(KeyCode::NumLock, true, bytes);
    second.Press(KeyCode::NumLock, false, bytes);
    REQUIRE(FeatherOS::Input::NumLock());
    REQUIRE(FeatherOS::Input::LedState() == 3);
    REQUIRE(led_changes == std::vector<uint8_t>{2, 3});
    REQUIRE(first.Press(KeyCode::KeypadEnter, true, bytes) == 1);
    first.Press(79, true, bytes);
    REQUIRE(bytes[0] == '1');

    // Each physical press toggles: the second keyboard turns Caps Lock off again.
    second.Press(KeyCode::CapsLock, true, bytes);
    second.Press(KeyCode::CapsLock, false, bytes);
    REQUIRE_FALSE(FeatherOS::Input::CapsLock());
    REQUIRE(led_changes == std::vector<uint8_t>{2, 3, 1});

    // Setting what is already set sends nothing; a change does.
    FeatherOS::Input::SetLockState(false, true);
    REQUIRE(led_changes.size() == 3);
    FeatherOS::Input::SetLockState(true, false);
    REQUIRE(led_changes == std::vector<uint8_t>{2, 3, 1, 2});

    FeatherOS::Input::SetLedCallback(nullptr);
    FeatherOS::Input::SetLockState(false, false);
    REQUIRE(led_changes.size() == 4);
}
