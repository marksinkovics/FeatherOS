#include <catch2/catch_test_macros.hpp>

#include "kernel/arch/serial.h"
#include "kernel/input/input.h"
#include "kernel/input/keyboard.h"
#include "kernel/terminal/terminal.h"
#include "support/scheduler_stubs.h"
#include "support/terminal_device.h"

#include <deque>
#include <string>

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
