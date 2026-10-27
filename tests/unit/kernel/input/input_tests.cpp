#include <catch2/catch_test_macros.hpp>

#include "kernel/arch/interrupts.h"
#include "kernel/arch/serial.h"
#include "kernel/input/input.h"
#include "kernel/scheduler.h"
#include "support/scheduler_stubs.h"

#include <deque>
#include <string>

namespace
{

std::deque<char> serial_input;

void DrainInput()
{
    char byte;
    while (FeatherOS::Input::TryReadByte(&byte))
    {
    }
}

std::string ReadAll()
{
    std::string text;
    char byte;
    while (FeatherOS::Input::TryReadByte(&byte))
    {
        text += byte;
    }
    return text;
}

} // namespace

namespace FeatherOS::Arch::Interrupts
{

uint64_t SaveAndDisable()
{
    return 0;
}

void Restore(uint64_t)
{
}

} // namespace FeatherOS::Arch::Interrupts


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

TEST_CASE("serial input arrives as typed, with CR LF counted once")
{
    DrainInput();
    serial_input = {'a', '\r', '\n', 'b', '\x7f', '\x1b', '[', 'A', '\n', '\n'};
    FeatherOS::Input::Poll();
    REQUIRE(ReadAll() == "a\rb\x7f\x1b[A\n\n");
    char byte;
    REQUIRE_FALSE(FeatherOS::Input::TryReadByte(&byte));
    REQUIRE_FALSE(FeatherOS::Input::TryReadByte(nullptr));
}

TEST_CASE("input queue is bounded and keeps a key's bytes together")
{
    DrainInput();
    std::string expected;
    for (uint32_t index = 0; index < 254; ++index)
    {
        const char byte = static_cast<char>('a' + index % 26);
        FeatherOS::Input::SubmitBytes(&byte, 1);
        expected += byte;
    }
    // Three bytes do not fit in the two left, so none of them is queued.
    FeatherOS::Input::SubmitBytes("\x1b[A", 3);
    FeatherOS::Input::SubmitBytes("xy", 2);
    expected += "xy";
    FeatherOS::Input::SubmitBytes("z", 1);
    REQUIRE(ReadAll() == expected);
}

TEST_CASE("input arrival wakes waiting readers")
{
    DrainInput();
    TestScheduler::wake_count = 0;
    FeatherOS::Input::SubmitBytes("\r", 1);
    REQUIRE(TestScheduler::wake_count == 1);

    serial_input.clear();
    FeatherOS::Input::Poll();
    REQUIRE(TestScheduler::wake_count == 1);

    serial_input = {'x'};
    FeatherOS::Input::Poll();
    REQUIRE(TestScheduler::wake_count == 2);
    DrainInput();
}

TEST_CASE("waiting for input blocks only while the queue is empty")
{
    DrainInput();
    TestScheduler::wait_count = 0;
    FeatherOS::Input::WaitForInput(250);
    REQUIRE(TestScheduler::wait_count == 1);
    REQUIRE(TestScheduler::last_wait_deadline == 250);
    REQUIRE(TestScheduler::lock_held_during_wait);

    FeatherOS::Input::SubmitBytes("q", 1);
    FeatherOS::Input::WaitForInput(0);
    REQUIRE(TestScheduler::wait_count == 1);
    DrainInput();
}

TEST_CASE("Ctrl+C goes to the interrupt handler, or is queued in order when it declines")
{
    DrainInput();
    static int handler_calls = 0;
    static bool consume = true;
    FeatherOS::Input::SetInterruptHandler(
        []
        {
            ++handler_calls;
            return consume;
        });

    serial_input = {'a', '\x03', 'b'};
    FeatherOS::Input::Poll();
    REQUIRE(handler_calls == 1);
    REQUIRE(ReadAll() == "ab");

    consume = false;
    FeatherOS::Input::SubmitBytes("\x03", 1);
    REQUIRE(handler_calls == 2);
    serial_input = {'x', '\x03', 'q'};
    FeatherOS::Input::Poll();
    REQUIRE(handler_calls == 3);
    REQUIRE(ReadAll() == "\x03x\x03q");

    FeatherOS::Input::SetInterruptHandler(nullptr);
    serial_input = {'\x03'};
    FeatherOS::Input::Poll();
    REQUIRE(handler_calls == 3);
    REQUIRE(ReadAll() == "\x03");
}
