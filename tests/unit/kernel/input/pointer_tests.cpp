#include <catch2/catch_test_macros.hpp>

#include "kernel/input/pointer.h"
#include "support/scheduler_stubs.h"
#include "user/syscall_abi.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <thread>
#include <vector>

namespace
{

namespace Pointer = FeatherOS::Input::Pointer;
namespace User = FeatherOS::User;
using Event = User::InputEvent;

constexpr uint64_t EventSize = sizeof(Event);

Event Make(uint16_t type, uint16_t code, int32_t value)
{
    Event event = {};
    event.type = type;
    event.code = code;
    event.value = value;
    return event;
}

Event Move(int32_t x)
{
    return Make(User::InputEventRelative, User::InputRelativeX, x);
}

Event Sync()
{
    return Make(User::InputEventSync, User::InputSyncReport, 0);
}

// The report a producer submits for a move by x, y: two moves and the end.
void SubmitMove(int32_t x)
{
    const Event report[] = {Move(x), Make(User::InputEventRelative, User::InputRelativeY, -x),
                            Sync()};
    Pointer::Submit(report, 3);
}

std::vector<Event> ReadAll(Pointer::Reader& reader, uint64_t bytes = 4096)
{
    std::vector<Event> events;
    std::vector<uint8_t> buffer(bytes);
    for (;;)
    {
        const uint64_t count = Pointer::Read(reader, buffer.data(), bytes, true);
        if (count == User::SyscallErrorWouldBlock)
        {
            return events;
        }
        REQUIRE_FALSE(User::IsSyscallError(count));
        REQUIRE(count % EventSize == 0);
        for (uint64_t offset = 0; offset < count; offset += EventSize)
        {
            Event event;
            std::memcpy(&event, buffer.data() + offset, EventSize);
            events.push_back(event);
        }
    }
}

bool IsDropped(const Event& event)
{
    return event.type == User::InputEventSync && event.code == User::InputSyncDropped;
}

bool interrupt_now = false;

bool Interrupted()
{
    return interrupt_now;
}

TEST_CASE("pointer T25: readers get whole reports in order, stamped with the uptime", "[pointer]")
{
    TestScheduler::Reset();
    Pointer::Reader reader = Pointer::Open();
    TestScheduler::ticks = 250;
    SubmitMove(3);
    SubmitMove(-4);
    const Event buttons[] = {Make(User::InputEventKey, User::InputButtonLeft, 1), Sync()};
    Pointer::Submit(buttons, 2);

    const std::vector<Event> events = ReadAll(reader);
    REQUIRE(events.size() == 8);
    REQUIRE(events[0].type == User::InputEventRelative);
    REQUIRE(events[0].code == User::InputRelativeX);
    REQUIRE(events[0].value == 3);
    REQUIRE(events[1].code == User::InputRelativeY);
    REQUIRE(events[1].value == -3);
    REQUIRE(events[2].type == User::InputEventSync);
    REQUIRE(events[3].value == -4);
    REQUIRE(events[6].type == User::InputEventKey);
    REQUIRE(events[6].code == 0x110);
    REQUIRE(events[6].value == 1);
    REQUIRE(events[7].code == User::InputSyncReport);
    for (const Event& event : events)
    {
        REQUIRE(event.seconds == 2);
        REQUIRE(event.microseconds == 500000);
    }
}

TEST_CASE("pointer T25: every reader has its own cursor and a new one starts at the end",
          "[pointer]")
{
    TestScheduler::Reset();
    Pointer::Reader first = Pointer::Open();
    SubmitMove(1);
    Pointer::Reader second = Pointer::Open();
    SubmitMove(2);

    REQUIRE(ReadAll(first).size() == 6);
    const std::vector<Event> later = ReadAll(second);
    REQUIRE(later.size() == 3);
    REQUIRE(later[0].value == 2);
    REQUIRE(ReadAll(first).empty());
    REQUIRE(ReadAll(second).empty());
    SubmitMove(3);
    REQUIRE(ReadAll(first).size() == 3);
    REQUIRE(ReadAll(second).size() == 3);
}

TEST_CASE("pointer T25: a short buffer returns whole events and the rest comes next", "[pointer]")
{
    TestScheduler::Reset();
    Pointer::Reader reader = Pointer::Open();
    SubmitMove(7);
    uint8_t buffer[EventSize * 2 + 23];
    REQUIRE(Pointer::Read(reader, buffer, sizeof(buffer), true) == EventSize * 2);
    REQUIRE(Pointer::Read(reader, buffer, sizeof(buffer), true) == EventSize);
    REQUIRE(Pointer::Read(reader, buffer, sizeof(buffer), true) == User::SyscallErrorWouldBlock);
}

TEST_CASE("pointer I28/T25: a read below one event is invalid, even with events queued",
          "[pointer]")
{
    TestScheduler::Reset();
    Pointer::Reader reader = Pointer::Open();
    uint8_t buffer[64];
    for (const uint64_t bytes : {uint64_t{0}, uint64_t{1}, uint64_t{10}, EventSize - 1})
    {
        REQUIRE(Pointer::Read(reader, buffer, bytes, true) == User::SyscallErrorInvalidArgument);
        REQUIRE(Pointer::Read(reader, buffer, bytes, false) == User::SyscallErrorInvalidArgument);
    }
    SubmitMove(1);
    REQUIRE(Pointer::Read(reader, buffer, 10, true) == User::SyscallErrorInvalidArgument);
    REQUIRE(Pointer::Read(reader, buffer, EventSize, true) == EventSize);
}

TEST_CASE("pointer I28/T25: an empty queue gives EAGAIN, or blocks until a report or a signal",
          "[pointer]")
{
    TestScheduler::Reset();
    Pointer::Reader reader = Pointer::Open();
    Event event;
    REQUIRE(Pointer::Read(reader, &event, EventSize, true) == User::SyscallErrorWouldBlock);

    TestScheduler::on_wait = [] { SubmitMove(5); };
    REQUIRE(Pointer::Read(reader, &event, EventSize, false) == EventSize);
    REQUIRE(event.value == 5);
    REQUIRE(TestScheduler::wait_count == 1);
    TestScheduler::on_wait = nullptr;
    REQUIRE(ReadAll(reader).size() == 2);

    interrupt_now = true;
    REQUIRE(Pointer::Read(reader, &event, EventSize, false, Interrupted) ==
            User::SyscallErrorInterrupted);
    interrupt_now = false;
    REQUIRE(TestScheduler::wait_count == 1);
}

TEST_CASE("pointer I28/T25: poll is readable exactly while the cursor is behind", "[pointer]")
{
    TestScheduler::Reset();
    Pointer::Reader reader = Pointer::Open();
    REQUIRE(Pointer::PollEvents(reader) == 0);
    SubmitMove(1);
    REQUIRE(Pointer::PollEvents(reader) == User::PollReadable);
    Event event;
    REQUIRE(Pointer::Read(reader, &event, EventSize, true) == EventSize);
    REQUIRE(Pointer::PollEvents(reader) == User::PollReadable);
    REQUIRE(Pointer::Read(reader, &event, EventSize, true) == EventSize);
    REQUIRE(Pointer::Read(reader, &event, EventSize, true) == EventSize);
    REQUIRE(Pointer::PollEvents(reader) == 0);
    Pointer::Reader other = Pointer::Open();
    REQUIRE(Pointer::PollEvents(other) == 0);
    SubmitMove(2);
    REQUIRE(Pointer::PollEvents(reader) == User::PollReadable);
    REQUIRE(Pointer::PollEvents(other) == User::PollReadable);
}

TEST_CASE("pointer: every submit wakes readers and poll", "[pointer]")
{
    TestScheduler::Reset();
    SubmitMove(1);
    REQUIRE(TestScheduler::wake_count == 1);
}

TEST_CASE("pointer I27/T25: overflow gives one SYN_DROPPED and then the next report start",
          "[pointer]")
{
    TestScheduler::Reset();
    Pointer::Reader behind = Pointer::Open();
    Pointer::Reader current = Pointer::Open();
    // 100 reports of 3 events: 300 events, so the ring (256) keeps the last 85 whole reports.
    std::vector<Event> seen;
    for (int report = 1; report <= 100; ++report)
    {
        SubmitMove(report);
        if (report % 10 == 0)
        {
            for (const Event& event : ReadAll(current))
            {
                seen.push_back(event);
            }
        }
    }
    REQUIRE(seen.size() == 300);
    for (const Event& event : seen)
    {
        REQUIRE_FALSE(IsDropped(event));
    }

    const std::vector<Event> events = ReadAll(behind);
    REQUIRE(IsDropped(events[0]));
    REQUIRE(events.size() == 1 + 85 * 3);
    int dropped = 0;
    for (const Event& event : events)
    {
        dropped += IsDropped(event) ? 1 : 0;
    }
    REQUIRE(dropped == 1);
    for (size_t report = 0; report < 85; ++report)
    {
        const Event* first = &events[1 + report * 3];
        REQUIRE(first[0].code == User::InputRelativeX);
        REQUIRE(first[0].value == static_cast<int32_t>(16 + report));
        REQUIRE(first[1].code == User::InputRelativeY);
        REQUIRE(first[2].code == User::InputSyncReport);
    }
    REQUIRE(ReadAll(behind).empty());
}

TEST_CASE("pointer I27/T25: reports of different sizes are dropped whole, from a mid-report cursor",
          "[pointer]")
{
    TestScheduler::Reset();
    Pointer::Reader reader = Pointer::Open();
    SubmitMove(1);
    Event one;
    REQUIRE(Pointer::Read(reader, &one, EventSize, true) == EventSize);
    // The reader now sits inside report 1. Reports of 2 and 5 events push it out of the ring.
    for (int round = 0; round < 80; ++round)
    {
        const Event two[] = {Make(User::InputEventKey, User::InputButtonLeft, round % 2), Sync()};
        Pointer::Submit(two, 2);
        const Event five[] = {Move(round), Move(round), Move(round), Move(round), Sync()};
        Pointer::Submit(five, 5);
    }
    const std::vector<Event> events = ReadAll(reader);
    REQUIRE(IsDropped(events[0]));
    // After the marker the events are whole reports: each ends with SYN_REPORT, and the ring
    // never holds more than RingSize events.
    REQUIRE(events.size() <= 1 + Pointer::RingSize);
    REQUIRE(events.back().code == User::InputSyncReport);
    bool start = true;
    for (size_t index = 1; index < events.size(); ++index)
    {
        if (start)
        {
            REQUIRE((events[index].type == User::InputEventKey ||
                     events[index].type == User::InputEventRelative));
        }
        start = events[index].type == User::InputEventSync &&
                events[index].code == User::InputSyncReport;
    }
    REQUIRE(start);
    // A reader at the end of what is left misses nothing further.
    SubmitMove(9);
    REQUIRE(ReadAll(reader).size() == 3);
}

TEST_CASE("pointer: a submit that is not a whole report is ignored", "[pointer]")
{
    TestScheduler::Reset();
    Pointer::Reader reader = Pointer::Open();
    const Event open_ended[] = {Move(1), Move(2)};
    Pointer::Submit(open_ended, 2);
    Pointer::Submit(open_ended, 0);
    std::vector<Event> huge(Pointer::RingSize + 1, Move(1));
    huge.back() = Sync();
    Pointer::Submit(huge.data(), huge.size());
    REQUIRE(Pointer::PollEvents(reader) == 0);
    std::vector<Event> full(Pointer::RingSize, Move(1));
    full.back() = Sync();
    Pointer::Submit(full.data(), full.size());
    REQUIRE(ReadAll(reader).size() == Pointer::RingSize);
}

TEST_CASE("pointer T29: one producer and three readers see ordered reports", "[pointer][concurrency]")
{
    TestScheduler::Reset();
    constexpr int Reports = 3000;
    struct Result
    {
        bool ordered = true;
        int last = 0;
        int dropped = 0;
    };
    Pointer::Reader readers[3] = {Pointer::Open(), Pointer::Open(), Pointer::Open()};
    Result results[3];
    std::vector<std::thread> threads;
    for (int index = 0; index < 3; ++index)
    {
        threads.emplace_back(
            [&, index]
            {
                Result& result = results[index];
                std::vector<uint8_t> buffer(EventSize * (3 + 7 * index));
                // 0: expecting X, 1: expecting Y, 2: expecting the end of the report.
                int expected = 0;
                int x = 0;
                while (result.last != Reports)
                {
                    const uint64_t bytes =
                        Pointer::Read(readers[index], buffer.data(), buffer.size(), false);
                    if (User::IsSyscallError(bytes) || bytes % EventSize != 0)
                    {
                        result.ordered = false;
                        return;
                    }
                    for (uint64_t offset = 0; offset < bytes; offset += EventSize)
                    {
                        Event event;
                        std::memcpy(&event, buffer.data() + offset, EventSize);
                        if (IsDropped(event))
                        {
                            ++result.dropped;
                            expected = 0;
                        }
                        else if (expected == 0 && event.code == User::InputRelativeX &&
                                 event.value > x)
                        {
                            x = event.value;
                            expected = 1;
                        }
                        else if (expected == 1 && event.code == User::InputRelativeY &&
                                 event.value == -x)
                        {
                            expected = 2;
                        }
                        else if (expected == 2 && event.type == User::InputEventSync &&
                                 event.code == User::InputSyncReport)
                        {
                            result.last = x;
                            expected = 0;
                        }
                        else
                        {
                            result.ordered = false;
                            return;
                        }
                    }
                }
            });
    }
    for (int report = 1; report <= Reports; ++report)
    {
        SubmitMove(report);
    }
    for (std::thread& thread : threads)
    {
        thread.join();
    }
    for (const Result& result : results)
    {
        REQUIRE(result.ordered);
        REQUIRE(result.last == Reports);
    }
}

} // namespace
