#include <catch2/catch_test_macros.hpp>

#include "kernel/framebuffer/graphics_mode.h"

#include <algorithm>
#include <cstdint>
#include <map>
#include <random>
#include <vector>

namespace
{

using FeatherOS::Console::Count;
using FeatherOS::GraphicsMode::Clip;
using FeatherOS::GraphicsMode::Clipped;
using FeatherOS::GraphicsMode::EventQueue;
using FeatherOS::GraphicsMode::KeyEventLimit;
using FeatherOS::GraphicsMode::Owners;
using FeatherOS::Input::KeyEvent;
using FeatherOS::Input::KeyTransition;
using FeatherOS::User::FramebufferEvent;
using FeatherOS::User::FramebufferEventHidden;
using FeatherOS::User::FramebufferEventPress;
using FeatherOS::User::FramebufferEventRelease;
using FeatherOS::User::FramebufferEventRepeat;
using FeatherOS::User::FramebufferEventShown;
using FeatherOS::User::FramebufferPresentRequest;
using FeatherOS::User::SyscallErrorBusy;
using FeatherOS::User::SyscallErrorInvalidArgument;

constexpr uint32_t ScreenWidth = 1024;
constexpr uint32_t ScreenHeight = 768;
constexpr int32_t Largest = INT32_MAX;
constexpr int32_t Smallest = INT32_MIN;

FramebufferPresentRequest Request(int32_t x, int32_t y, uint32_t width, uint32_t height,
                                  uint32_t stride, uint32_t flags = 0)
{
    return {0x1000, stride, x, y, width, height, flags};
}

struct Expected
{
    FramebufferPresentRequest request;
    uint64_t result;
    uint32_t screen_x;
    uint32_t screen_y;
    uint32_t width;
    uint32_t height;
    uint64_t source_offset;
};

} // namespace

TEST_CASE("Clip checks the request's sizes, then keeps the visible part and where it starts")
{
    const Expected table[] = {
        {Request(0, 0, 1024, 768, 1024), 0, 0, 0, 1024, 768, 0},
        {Request(0, 0, 0, 10, 0), 0, 0, 0, 0, 0, 0},
        {Request(5, 5, 10, 0, 10), 0, 0, 0, 0, 0, 0},
        {Request(0, 0, 20, 10, 10), SyscallErrorInvalidArgument, 0, 0, 0, 0, 0},
        {Request(0, 0, 10, 10, 16385), SyscallErrorInvalidArgument, 0, 0, 0, 0, 0},
        {Request(0, 0, 20000, 10, 20000), SyscallErrorInvalidArgument, 0, 0, 0, 0, 0},
        {Request(0, 0, 10, 16385, 10), SyscallErrorInvalidArgument, 0, 0, 0, 0, 0},
        {Request(0, 0, 10, 10, 10, 1), SyscallErrorInvalidArgument, 0, 0, 0, 0, 0},
        {Request(0, 0, 0, 0, 0, 1), SyscallErrorInvalidArgument, 0, 0, 0, 0, 0},
        {Request(-10, 0, 20, 4, 20), 0, 0, 0, 10, 4, 10},
        {Request(0, 760, 16, 100, 16), 0, 0, 760, 16, 8, 0},
        {Request(1000, 700, 100, 100, 100), 0, 1000, 700, 24, 68, 0},
        {Request(-5, -10, 20, 30, 30), 0, 0, 0, 15, 20, 305},
        {Request(1023, 767, 50, 50, 64), 0, 1023, 767, 1, 1, 0},
        {Request(1024, 0, 10, 10, 10), 0, 0, 0, 0, 0, 0},
        {Request(0, 768, 10, 10, 10), 0, 0, 0, 0, 0, 0},
        {Request(-20, 0, 20, 10, 20), 0, 0, 0, 0, 0, 0},
        {Request(0, -10, 20, 10, 20), 0, 0, 0, 0, 0, 0},
        {Request(Largest, Largest, 16384, 16384, 16384), 0, 0, 0, 0, 0, 0},
        {Request(Smallest, Smallest, 16384, 16384, 16384), 0, 0, 0, 0, 0, 0},
        {Request(Largest, 0, 16384, 10, 16384), 0, 0, 0, 0, 0, 0},
        {Request(0, Smallest, 10, 16384, 10), 0, 0, 0, 0, 0, 0},
        {Request(-Largest, -Largest, 16384, 16384, 16384), 0, 0, 0, 0, 0, 0},
        {Request(-16383, -16383, 16384, 16384, 16384), 0, 0, 0, 1, 1, 16383ULL * 16384 + 16383},
        {Request(0, -16383, 16384, 16384, 16384), 0, 0, 0, 1024, 1, 16383ULL * 16384},
    };
    for (const Expected& expected : table)
    {
        Clipped clipped = {1, 2, 3, 4, 5};
        INFO("x " << expected.request.x << " y " << expected.request.y << " width "
                  << expected.request.width << " height " << expected.request.height
                  << " stride " << expected.request.stride << " flags " << expected.request.flags);
        REQUIRE(Clip(expected.request, ScreenWidth, ScreenHeight, &clipped) == expected.result);
        REQUIRE(clipped.screen_x == expected.screen_x);
        REQUIRE(clipped.screen_y == expected.screen_y);
        REQUIRE(clipped.width == expected.width);
        REQUIRE(clipped.height == expected.height);
        REQUIRE(clipped.source_offset == expected.source_offset);
    }
}

TEST_CASE("Clip's source offset reaches the last visible pixel inside the request")
{
    for (const int32_t x : {-1000, -5, 0, 500, 1000})
    {
        for (const int32_t y : {-700, -1, 0, 400, 767})
        {
            const FramebufferPresentRequest request = Request(x, y, 300, 200, 350);
            Clipped clipped;
            REQUIRE(Clip(request, ScreenWidth, ScreenHeight, &clipped) == 0);
            if (clipped.width == 0 || clipped.height == 0)
            {
                continue;
            }
            const uint64_t last = clipped.source_offset +
                                  static_cast<uint64_t>(clipped.height - 1) * request.stride +
                                  (clipped.width - 1);
            REQUIRE(last < static_cast<uint64_t>(request.stride) * (request.height - 1) +
                               request.width);
            REQUIRE(clipped.screen_x + clipped.width <= ScreenWidth);
            REQUIRE(clipped.screen_y + clipped.height <= ScreenHeight);
            REQUIRE(static_cast<int64_t>(clipped.screen_x) - x ==
                    static_cast<int64_t>(clipped.source_offset % request.stride));
        }
    }
}

TEST_CASE("an owner takes one console, and a console has one owner")
{
    Owners owners;
    int first;
    int second;
    REQUIRE(owners.Acquire(0, &first, true, 0) == 0);
    REQUIRE(owners.Acquire(0, &first, true, 0) == 0);
    REQUIRE(owners.Acquire(0, &second, true, 0) == SyscallErrorBusy);
    REQUIRE(owners.Acquire(1, &first, true, 0) == SyscallErrorBusy);
    REQUIRE(owners.Acquire(Count, &second, true, 0) == SyscallErrorInvalidArgument);
    REQUIRE(owners.Acquire(1, nullptr, true, 0) == SyscallErrorInvalidArgument);
    REQUIRE(owners.Owns(0, &first));
    REQUIRE_FALSE(owners.Owns(0, &second));
    REQUIRE_FALSE(owners.Owns(1, &first));
    REQUIRE_FALSE(owners.Owns(0, nullptr));
    REQUIRE(owners.Acquired(0));
    REQUIRE_FALSE(owners.Acquired(1));
    REQUIRE(owners.ConsoleOf(&first) == 0);
    REQUIRE(owners.ConsoleOf(&second) == Count);

    REQUIRE(owners.Release(&second) == Count);
    REQUIRE(owners.Acquired(0));
    REQUIRE(owners.Release(&first) == 0);
    REQUIRE(owners.Release(&first) == Count);
    REQUIRE_FALSE(owners.Acquired(0));
    REQUIRE(owners.Acquire(0, &second, true, 0) == 0);
}

TEST_CASE("the consoles' owners are independent")
{
    Owners owners;
    int tokens[Count];
    for (uint32_t console = 0; console < Count; ++console)
    {
        REQUIRE(owners.Acquire(console, &tokens[console], true, 0) == 0);
    }
    for (uint32_t console = 0; console < Count; ++console)
    {
        REQUIRE(owners.Owns(console, &tokens[console]));
        REQUIRE(owners.Owns(console, &tokens[(console + 1) % Count]) == false);
    }
    REQUIRE(owners.Release(&tokens[2]) == 2);
    for (uint32_t console = 0; console < Count; ++console)
    {
        REQUIRE(owners.Acquired(console) == (console != 2));
    }
    REQUIRE(owners.Acquire(2, &tokens[0], true, 0) == SyscallErrorBusy);
    REQUIRE(owners.Acquire(2, &tokens[2], true, 0) == 0);
}

namespace
{

FramebufferEvent Event(uint16_t code, uint8_t kind, uint8_t modifiers = 0, uint32_t time_ms = 0)
{
    return {code, kind, modifiers, time_ms};
}

std::vector<FramebufferEvent> Drain(Owners& owners, const void* owner)
{
    std::vector<FramebufferEvent> events;
    FramebufferEvent buffer[7];
    for (;;)
    {
        const uint64_t taken = owners.TakeEvents(owner, buffer, 7);
        REQUIRE_FALSE(FeatherOS::User::IsSyscallError(taken));
        if (taken == 0)
        {
            return events;
        }
        events.insert(events.end(), buffer, buffer + taken);
    }
}

struct Shape
{
    uint16_t code;
    uint8_t kind;
};

std::vector<Shape> Shapes(const std::vector<FramebufferEvent>& events)
{
    std::vector<Shape> shapes;
    for (const FramebufferEvent& event : events)
    {
        shapes.push_back({event.code, event.kind});
    }
    return shapes;
}

bool operator==(const Shape& left, const Shape& right)
{
    return left.code == right.code && left.kind == right.kind;
}

KeyEvent Key(uint16_t code, KeyTransition transition, uint8_t modifiers = 0)
{
    return {code, transition, modifiers};
}

} // namespace

TEST_CASE("EventQueue is first in, first out across the wrap")
{
    EventQueue queue;
    FramebufferEvent taken[64];
    uint32_t next_push = 0;
    uint32_t next_pop = 0;
    for (uint32_t round = 0; round < 5; ++round)
    {
        for (uint32_t index = 0; index < 40; ++index)
        {
            REQUIRE(queue.Push(Event(static_cast<uint16_t>(next_push), 1, 0, next_push), 64));
            ++next_push;
        }
        const uint32_t count = queue.Pop(taken, 37);
        REQUIRE(count == 37);
        for (uint32_t index = 0; index < count; ++index)
        {
            REQUIRE(taken[index].time_ms == next_pop);
            ++next_pop;
        }
        REQUIRE(queue.Size() == next_push - next_pop);
        REQUIRE(queue.Pop(taken, 0) == 0);
    }
    REQUIRE(queue.Dropped() == 0);
}

TEST_CASE("EventQueue refuses and counts what does not fit, key events earlier than state ones")
{
    EventQueue queue;
    for (uint32_t index = 0; index < KeyEventLimit; ++index)
    {
        REQUIRE(queue.Push(Event(1, 1), KeyEventLimit));
    }
    REQUIRE(KeyEventLimit == 62);
    REQUIRE_FALSE(queue.Push(Event(2, 1), KeyEventLimit));
    REQUIRE_FALSE(queue.Push(Event(2, 1), KeyEventLimit));
    REQUIRE(queue.Dropped() == 2);
    REQUIRE(queue.Push(Event(0, FramebufferEventHidden), 64));
    REQUIRE(queue.Push(Event(0, FramebufferEventShown), 64));
    REQUIRE_FALSE(queue.Push(Event(0, FramebufferEventHidden), 64));
    REQUIRE(queue.Size() == 64);
    REQUIRE(queue.Dropped() == 3);

    FramebufferEvent taken[100];
    REQUIRE(queue.Pop(taken, 1) == 1);
    REQUIRE(taken[0].code == 1);
    REQUIRE(queue.Pop(taken, 100) == 63);
    REQUIRE(taken[61].kind == FramebufferEventHidden);
    REQUIRE(taken[62].kind == FramebufferEventShown);
    REQUIRE(queue.Size() == 0);
    REQUIRE(queue.Pop(taken, 100) == 0);
    REQUIRE(queue.Dropped() == 3);
    queue.Clear();
    REQUIRE(queue.Dropped() == 0);
    REQUIRE(queue.Push(Event(3, 1), 64));
    queue.Clear();
    REQUIRE(queue.Size() == 0);
}

TEST_CASE("an acquire queues Shown or Hidden, and a re-acquire by the owner queues nothing")
{
    Owners owners;
    int first;
    int second;
    REQUIRE(owners.Acquire(0, &first, true, 7) == 0);
    REQUIRE(owners.Acquire(2, &second, false, 9) == 0);
    REQUIRE(owners.Acquire(0, &first, true, 8) == 0);
    FramebufferEvent events[4];
    REQUIRE(owners.TakeEvents(&first, events, 4) == 1);
    REQUIRE(events[0].kind == FramebufferEventShown);
    REQUIRE(events[0].code == 0);
    REQUIRE(events[0].modifiers == 0);
    REQUIRE(events[0].time_ms == 7);
    REQUIRE(owners.TakeEvents(&second, events, 4) == 1);
    REQUIRE(events[0].kind == FramebufferEventHidden);
    REQUIRE(events[0].time_ms == 9);
    REQUIRE(owners.Acquire(0, &first, true, 8) == 0);
    REQUIRE(owners.PendingEvents(&first) == 0);
}

TEST_CASE("DeliverKey queues a Press for a key not seen and a Repeat for one seen")
{
    struct Row
    {
        KeyTransition transition;
        bool seen_before;
        bool queued;
        uint8_t kind;
        bool seen_after;
    };
    const Row table[] = {
        {KeyTransition::Press, false, true, FramebufferEventPress, true},
        {KeyTransition::Repeat, false, true, FramebufferEventPress, true},
        {KeyTransition::Press, true, true, FramebufferEventRepeat, true},
        {KeyTransition::Repeat, true, true, FramebufferEventRepeat, true},
        {KeyTransition::Release, true, true, FramebufferEventRelease, false},
        {KeyTransition::Release, false, false, 0, false},
        {KeyTransition::None, false, false, 0, false},
        {KeyTransition::None, true, false, 0, true},
    };
    for (const Row& row : table)
    {
        INFO("transition " << static_cast<int>(row.transition) << " seen " << row.seen_before);
        Owners owners;
        int owner;
        REQUIRE(owners.Acquire(1, &owner, true, 0) == 0);
        Drain(owners, &owner);
        if (row.seen_before)
        {
            REQUIRE(owners.DeliverKey(1, Key(30, KeyTransition::Press), 1));
            Drain(owners, &owner);
        }
        REQUIRE(owners.DeliverKey(1, Key(30, row.transition, 3), 5));
        const std::vector<FramebufferEvent> events = Drain(owners, &owner);
        REQUIRE(events.size() == (row.queued ? 1U : 0U));
        if (row.queued)
        {
            REQUIRE(events[0].code == 30);
            REQUIRE(events[0].kind == row.kind);
            REQUIRE(events[0].modifiers == 3);
            REQUIRE(events[0].time_ms == 5);
        }
        REQUIRE(owners.DroppedEvents(&owner) == 0);
        owners.Hide(1, 0, 6);
        const std::vector<FramebufferEvent> hidden = Drain(owners, &owner);
        REQUIRE(hidden.size() == (row.seen_after ? 2U : 1U));
    }
}

TEST_CASE("DeliverKey reports whether the console has an owner that is shown")
{
    Owners owners;
    int owner;
    REQUIRE_FALSE(owners.DeliverKey(0, Key(30, KeyTransition::Press), 0));
    REQUIRE(owners.Acquire(0, &owner, false, 0) == 0);
    REQUIRE_FALSE(owners.DeliverKey(0, Key(30, KeyTransition::Press), 0));
    owners.Show(0, 1);
    REQUIRE(owners.DeliverKey(0, Key(30, KeyTransition::Press), 2));
    REQUIRE_FALSE(owners.DeliverKey(0, Key(256, KeyTransition::Press), 2));
    REQUIRE_FALSE(owners.DeliverKey(Count, Key(30, KeyTransition::Press), 2));
    REQUIRE_FALSE(owners.DeliverKey(1, Key(30, KeyTransition::Press), 2));
    owners.Release(&owner);
    REQUIRE_FALSE(owners.DeliverKey(0, Key(30, KeyTransition::Press), 2));
}

TEST_CASE("Hide releases every key the owner saw, in code order, then says Hidden")
{
    Owners owners;
    int owner;
    REQUIRE(owners.Acquire(0, &owner, true, 0) == 0);
    for (const uint16_t code : {45, 30, 56})
    {
        REQUIRE(owners.DeliverKey(0, Key(code, KeyTransition::Press), 1));
    }
    REQUIRE(owners.DeliverKey(0, Key(45, KeyTransition::Release), 2));
    REQUIRE(owners.DeliverKey(0, Key(45, KeyTransition::Press), 3));
    Drain(owners, &owner);
    owners.Hide(0, 4, 10);
    const std::vector<FramebufferEvent> events = Drain(owners, &owner);
    REQUIRE(Shapes(events) == std::vector<Shape>{{30, FramebufferEventRelease},
                                                 {45, FramebufferEventRelease},
                                                 {56, FramebufferEventRelease},
                                                 {0, FramebufferEventHidden}});
    for (const FramebufferEvent& event : events)
    {
        REQUIRE(event.time_ms == 10);
        REQUIRE(event.modifiers == (event.kind == FramebufferEventHidden ? 0 : 4));
    }

    REQUIRE_FALSE(owners.DeliverKey(0, Key(30, KeyTransition::Repeat), 11));
    owners.Hide(0, 0, 12);
    REQUIRE(owners.PendingEvents(&owner) == 0);
    owners.Show(0, 13);
    owners.Show(0, 14);
    REQUIRE(Shapes(Drain(owners, &owner)) == std::vector<Shape>{{0, FramebufferEventShown}});
    REQUIRE(owners.DeliverKey(0, Key(30, KeyTransition::Repeat), 15));
    REQUIRE(owners.DeliverKey(0, Key(45, KeyTransition::Release), 15));
    REQUIRE(Shapes(Drain(owners, &owner)) == std::vector<Shape>{{30, FramebufferEventPress}});
}

TEST_CASE("Release discards the queue and the count, and a non-owner's TakeEvents fails")
{
    Owners owners;
    int owner;
    int stranger;
    FramebufferEvent events[4];
    REQUIRE(owners.TakeEvents(&owner, events, 4) == SyscallErrorInvalidArgument);
    REQUIRE(owners.Acquire(3, &owner, true, 0) == 0);
    for (uint32_t index = 0; index < 70; ++index)
    {
        REQUIRE(owners.DeliverKey(3, Key(30, KeyTransition::Press), index));
    }
    REQUIRE(owners.PendingEvents(&owner) == 62);
    REQUIRE(owners.DroppedEvents(&owner) == 9);
    REQUIRE(owners.TakeEvents(&stranger, events, 4) == SyscallErrorInvalidArgument);
    REQUIRE(owners.PendingEvents(&stranger) == 0);
    REQUIRE(owners.DroppedEvents(&stranger) == 0);
    REQUIRE(owners.Release(&owner) == 3);
    REQUIRE(owners.PendingEvents(&owner) == 0);
    REQUIRE(owners.DroppedEvents(&owner) == 0);
    REQUIRE(owners.TakeEvents(&owner, events, 4) == SyscallErrorInvalidArgument);

    REQUIRE(owners.Acquire(3, &owner, false, 4) == 0);
    REQUIRE(Shapes(Drain(owners, &owner)) == std::vector<Shape>{{0, FramebufferEventHidden}});
    owners.Show(3, 5);
    REQUIRE(owners.DeliverKey(3, Key(30, KeyTransition::Release), 6));
    REQUIRE(Shapes(Drain(owners, &owner)) == std::vector<Shape>{{0, FramebufferEventShown}});
}

TEST_CASE("four consoles queue their events independently")
{
    Owners owners;
    int tokens[Count];
    for (uint32_t console = 0; console < Count; ++console)
    {
        REQUIRE(owners.Acquire(console, &tokens[console], true, 0) == 0);
        Drain(owners, &tokens[console]);
    }
    REQUIRE(owners.DeliverKey(1, Key(30, KeyTransition::Press), 1));
    REQUIRE(owners.DeliverKey(3, Key(31, KeyTransition::Press), 1));
    REQUIRE(owners.DeliverKey(3, Key(32, KeyTransition::Press), 1));
    owners.Hide(3, 0, 2);
    REQUIRE(owners.PendingEvents(&tokens[0]) == 0);
    REQUIRE(owners.PendingEvents(&tokens[1]) == 1);
    REQUIRE(owners.PendingEvents(&tokens[2]) == 0);
    REQUIRE(owners.PendingEvents(&tokens[3]) == 5);
    owners.Release(&tokens[1]);
    REQUIRE(owners.PendingEvents(&tokens[3]) == 5);
    REQUIRE(owners.DeliverKey(3, Key(31, KeyTransition::Release), 3) == false);
}

TEST_CASE("an owner that never reads gets 62 key events, then drops are counted")
{
    Owners owners;
    int owner;
    REQUIRE(owners.Acquire(0, &owner, true, 0) == 0);
    Drain(owners, &owner);
    for (uint32_t index = 0; index < 40; ++index)
    {
        REQUIRE(owners.DeliverKey(0, Key(30, KeyTransition::Press), index));
        REQUIRE(owners.DeliverKey(0, Key(30, KeyTransition::Release), index));
    }
    // Refused presses are counted; the releases of keys never announced are not.
    REQUIRE(owners.PendingEvents(&owner) == 62);
    REQUIRE(owners.DroppedEvents(&owner) == 9);
    owners.Hide(0, 0, 100);
    REQUIRE(owners.PendingEvents(&owner) == 63);
    REQUIRE(owners.DroppedEvents(&owner) == 9);
    owners.Show(0, 101);
    REQUIRE(owners.PendingEvents(&owner) == 64);
    owners.Hide(0, 0, 102);
    REQUIRE(owners.DroppedEvents(&owner) == 10);

    const std::vector<FramebufferEvent> events = Drain(owners, &owner);
    REQUIRE(events.size() == 64);
    for (uint32_t index = 0; index < 62; ++index)
    {
        REQUIRE(events[index].kind ==
                (index % 2 == 0 ? FramebufferEventPress : FramebufferEventRelease));
    }
}

TEST_CASE("a release the queue refused is owed and queued once there is room")
{
    Owners owners;
    int owner;
    REQUIRE(owners.Acquire(0, &owner, true, 0) == 0);
    Drain(owners, &owner);
    REQUIRE(owners.DeliverKey(0, Key(30, KeyTransition::Press), 0));
    for (uint16_t code = 100; code < 161; ++code)
    {
        REQUIRE(owners.DeliverKey(0, Key(code, KeyTransition::Press), 0));
    }
    REQUIRE(owners.PendingEvents(&owner) == 62);
    REQUIRE(owners.DeliverKey(0, Key(30, KeyTransition::Release), 1));
    REQUIRE(owners.DroppedEvents(&owner) == 1);
    FramebufferEvent events[1];
    REQUIRE(owners.TakeEvents(&owner, events, 1) == 1);
    REQUIRE(owners.DeliverKey(0, Key(30, KeyTransition::Release), 2));
    REQUIRE(owners.DroppedEvents(&owner) == 1);
    const std::vector<FramebufferEvent> rest = Drain(owners, &owner);
    REQUIRE(rest.back().code == 30);
    REQUIRE(rest.back().kind == FramebufferEventRelease);
}

TEST_CASE("random sequences keep every key's stream well formed and the queue bounded")
{
    struct Consumer
    {
        bool stated = false;
        bool hidden = false;
        std::map<uint16_t, int> keys;
    };
    std::mt19937 random(12345);
    for (uint32_t sequence = 0; sequence < 10000; ++sequence)
    {
        Owners owners;
        int owner;
        const uint32_t console = random() % Count;
        REQUIRE(owners.Acquire(console, &owner, random() % 2 == 0, 0) == 0);
        Consumer consumer;
        bool acquired = true;
        const uint32_t steps = 20 + random() % 120;
        const auto check = [&](const FramebufferEvent& event)
        {
            if (event.kind == FramebufferEventShown)
            {
                REQUIRE((!consumer.stated || consumer.hidden));
                consumer.hidden = false;
                consumer.stated = true;
                return;
            }
            if (event.kind == FramebufferEventHidden)
            {
                REQUIRE((!consumer.stated || !consumer.hidden));
                for (const auto& [code, down] : consumer.keys)
                {
                    INFO("key " << code << " still down at Hidden");
                    REQUIRE(down == 0);
                }
                consumer.hidden = true;
                consumer.stated = true;
                return;
            }
            REQUIRE(consumer.stated);
            REQUIRE_FALSE(consumer.hidden);
            int& down = consumer.keys[event.code];
            if (event.kind == FramebufferEventPress)
            {
                REQUIRE(down == 0);
                down = 1;
            }
            else
            {
                REQUIRE(down == 1);
                if (event.kind == FramebufferEventRelease)
                {
                    down = 0;
                }
            }
        };
        for (uint32_t step = 0; step < steps && acquired; ++step)
        {
            const uint32_t action = random() % 12;
            const auto code = static_cast<uint16_t>(28 + random() % 6);
            if (action < 6)
            {
                const auto transition = static_cast<KeyTransition>(1 + random() % 3);
                owners.DeliverKey(console, Key(code, transition), step);
            }
            else if (action == 6)
            {
                owners.Hide(console, 0, step);
            }
            else if (action == 7)
            {
                owners.Show(console, step);
            }
            else if (action < 11)
            {
                const bool lossless = owners.DroppedEvents(&owner) == 0;
                FramebufferEvent events[5];
                const uint64_t taken = owners.TakeEvents(&owner, events, 1 + random() % 5);
                REQUIRE_FALSE(FeatherOS::User::IsSyscallError(taken));
                for (uint64_t index = 0; lossless && index < taken; ++index)
                {
                    check(events[index]);
                }
            }
            else if (random() % 4 == 0)
            {
                REQUIRE(owners.Release(&owner) == console);
                acquired = false;
            }
            if (acquired)
            {
                REQUIRE(owners.PendingEvents(&owner) <= 64);
            }
        }
    }
}

TEST_CASE("the Meta keys queue as any key does, and the Super bit rides on the keys after them")
{
    constexpr uint8_t Super = FeatherOS::Input::Modifier::Super;
    constexpr uint16_t LeftMeta = FeatherOS::Input::KeyCode::LeftMeta;
    Owners owners;
    int owner;
    REQUIRE(owners.Acquire(0, &owner, true, 0) == 0);
    Drain(owners, &owner);
    REQUIRE(owners.DeliverKey(0, Key(LeftMeta, KeyTransition::Press, Super), 1));
    REQUIRE(owners.DeliverKey(0, Key(LeftMeta, KeyTransition::Repeat, Super), 2));
    REQUIRE(owners.DeliverKey(0, Key(30, KeyTransition::Press, Super), 3));
    REQUIRE(owners.DeliverKey(0, Key(30, KeyTransition::Release, Super), 4));
    REQUIRE(owners.DeliverKey(0, Key(LeftMeta, KeyTransition::Release, 0), 5));
    const std::vector<FramebufferEvent> events = Drain(owners, &owner);
    REQUIRE(Shapes(events) == std::vector<Shape>{{LeftMeta, FramebufferEventPress},
                                                 {LeftMeta, FramebufferEventRepeat},
                                                 {30, FramebufferEventPress},
                                                 {30, FramebufferEventRelease},
                                                 {LeftMeta, FramebufferEventRelease}});
    const uint8_t modifiers[] = {Super, Super, Super, Super, 0};
    for (size_t index = 0; index < events.size(); ++index)
    {
        REQUIRE(events[index].modifiers == modifiers[index]);
    }

    REQUIRE(owners.DeliverKey(0, Key(LeftMeta, KeyTransition::Press, Super), 6));
    Drain(owners, &owner);
    owners.Hide(0, Super, 7);
    REQUIRE(Shapes(Drain(owners, &owner)) == std::vector<Shape>{{LeftMeta, FramebufferEventRelease},
                                                                {0, FramebufferEventHidden}});
}

TEST_CASE("RawKeys is true for a raw owner only, keeps the first flags and ends with the owner")
{
    constexpr uint64_t Raw = FeatherOS::User::FramebufferAcquireRawKeys;
    constexpr uint64_t Keep = FeatherOS::User::FramebufferAcquireKeepScreen;
    struct Row
    {
        uint64_t flags;
        bool raw;
    };
    for (const Row& row : {Row{0, false}, Row{Raw, true}, Row{Keep, false}, Row{Raw | Keep, true}})
    {
        INFO("flags " << row.flags);
        Owners owners;
        int owner;
        REQUIRE_FALSE(owners.RawKeys(0));
        REQUIRE(owners.Acquire(0, &owner, true, 0, row.flags) == 0);
        REQUIRE(owners.RawKeys(0) == row.raw);
        REQUIRE_FALSE(owners.RawKeys(1));
        REQUIRE(owners.Acquire(0, &owner, true, 0, row.raw ? 0 : Raw) == 0);
        REQUIRE(owners.RawKeys(0) == row.raw);
        REQUIRE(owners.Release(&owner) == 0);
        REQUIRE_FALSE(owners.RawKeys(0));
        REQUIRE_FALSE(owners.RawKeys(Count));
    }

    Owners owners;
    int first;
    int second;
    REQUIRE(owners.Acquire(0, &first, true, 0, Raw) == 0);
    REQUIRE(owners.Release(&first) == 0);
    REQUIRE(owners.Acquire(0, &second, true, 0) == 0);
    REQUIRE_FALSE(owners.RawKeys(0));
    REQUIRE(owners.Acquire(1, &first, true, 0, Raw) == 0);
    REQUIRE(owners.RawKeys(1));
    REQUIRE_FALSE(owners.RawKeys(0));
}
