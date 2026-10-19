#include <catch2/catch_test_macros.hpp>

#include "kernel/arch/interrupts.h"
#include "kernel/arch/serial.h"
#include "kernel/input/input.h"
#include "kernel/scheduler.h"

#include <deque>
#include <vector>

namespace
{

std::deque<char> serial_input;
int wake_count = 0;
int wait_count = 0;
uint64_t last_wait_deadline = 0;
bool lock_held_during_wait = false;

void DrainEvents()
{
    FeatherOS::Input::Event event;
    while (FeatherOS::Input::TryReadEvent(&event))
    {
    }
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

namespace FeatherOS::Scheduler
{

void WaitOn(WaitQueue&, Sync::SpinLock& lock, uint64_t deadline_tick)
{
    ++wait_count;
    last_wait_deadline = deadline_tick;
    lock_held_during_wait = lock.IsLocked();
}

void WakeAll(WaitQueue&)
{
    ++wake_count;
}

} // namespace FeatherOS::Scheduler

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

TEST_CASE("serial input becomes normalized key events")
{
    DrainEvents();
    serial_input = {'a', '\r', '\n', 'b', '\b', '\x7f', '\t', 'Z'};
    FeatherOS::Input::Poll();

    FeatherOS::Input::Event event;
    REQUIRE(FeatherOS::Input::TryReadEvent(&event));
    REQUIRE(event.type == FeatherOS::Input::EventType::Character);
    REQUIRE(event.character == 'a');

    REQUIRE(FeatherOS::Input::TryReadEvent(&event));
    REQUIRE(event.type == FeatherOS::Input::EventType::Enter);

    REQUIRE(FeatherOS::Input::TryReadEvent(&event));
    REQUIRE(event.type == FeatherOS::Input::EventType::Character);
    REQUIRE(event.character == 'b');

    REQUIRE(FeatherOS::Input::TryReadEvent(&event));
    REQUIRE(event.type == FeatherOS::Input::EventType::Backspace);
    REQUIRE(FeatherOS::Input::TryReadEvent(&event));
    REQUIRE(event.type == FeatherOS::Input::EventType::Backspace);

    REQUIRE(FeatherOS::Input::TryReadEvent(&event));
    REQUIRE(event.type == FeatherOS::Input::EventType::Character);
    REQUIRE(event.character == 'Z');
    REQUIRE_FALSE(FeatherOS::Input::TryReadEvent(&event));
}

TEST_CASE("input event queue is bounded and preserves FIFO order")
{
    DrainEvents();
    for (uint32_t index = 0; index < 67; ++index)
    {
        FeatherOS::Input::SubmitEvent(
            {FeatherOS::Input::EventType::Character, static_cast<char>('a' + index % 26)});
    }

    FeatherOS::Input::Event event;
    for (uint32_t index = 0; index < 64; ++index)
    {
        REQUIRE(FeatherOS::Input::TryReadEvent(&event));
        REQUIRE(event.type == FeatherOS::Input::EventType::Character);
        REQUIRE(event.character == static_cast<char>('a' + index % 26));
    }
    REQUIRE_FALSE(FeatherOS::Input::TryReadEvent(&event));
    REQUIRE_FALSE(FeatherOS::Input::TryReadEvent(nullptr));
}
TEST_CASE("input arrival wakes waiting readers")
{
    DrainEvents();
    wake_count = 0;
    FeatherOS::Input::SubmitEvent({FeatherOS::Input::EventType::Enter, '\0'});
    REQUIRE(wake_count == 1);

    serial_input.clear();
    FeatherOS::Input::Poll();
    REQUIRE(wake_count == 1);

    serial_input = {'x'};
    FeatherOS::Input::Poll();
    REQUIRE(wake_count == 2);
    DrainEvents();
}

TEST_CASE("waiting for input blocks only while the queue is empty")
{
    DrainEvents();
    wait_count = 0;
    FeatherOS::Input::WaitForEvent(250);
    REQUIRE(wait_count == 1);
    REQUIRE(last_wait_deadline == 250);
    REQUIRE(lock_held_during_wait);

    FeatherOS::Input::SubmitEvent({FeatherOS::Input::EventType::Character, 'q'});
    FeatherOS::Input::WaitForEvent(0);
    REQUIRE(wait_count == 1);
    DrainEvents();
}
