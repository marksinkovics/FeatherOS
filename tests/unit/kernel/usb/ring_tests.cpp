#include <catch2/catch_test_macros.hpp>

#include "fake_xhci.h"

#include "kernel/usb/xhci/registers.h"
#include "kernel/usb/xhci/ring.h"

#include <cstdint>
#include <vector>

namespace
{

namespace X = FeatherOS::Usb::Xhci;
using FeatherOS::Memory::Dma::Buffer;
using FeatherOS::Usb::Testing::FakeXhci;
using FeatherOS::Usb::Testing::PhysicalMemory;
using X::Trb;

constexpr uint64_t DataAddress = 0x1000;

class Rig
{
public:
    Rig()
    {
        command_buffer = memory.Allocate(X::RingBytes);
        transfer_buffer = memory.Allocate(X::RingBytes);
        event_buffer = memory.Allocate(X::RingBytes);
        Buffer table = memory.Allocate(64);
        REQUIRE(commands.Initialize(command_buffer, X::TrbType::NoOpCommand));
        REQUIRE(transfers.Initialize(transfer_buffer));
        REQUIRE(events.Initialize(event_buffer));
        auto* entry = reinterpret_cast<volatile uint32_t*>(table.address);
        entry[0] = static_cast<uint32_t>(event_buffer.physical);
        entry[1] = static_cast<uint32_t>(event_buffer.physical >> 32);
        entry[2] = X::RingTrbs;
        hardware.Write32(Runtime(X::Runtime::ErstSize), 1);
        hardware.Write64(Runtime(X::Runtime::ErstBase), table.physical);
        hardware.Write64(FakeXhci::OperationalOffset(X::Op::CommandRing),
                         commands.Physical() | X::CommandRingControl::RingCycleState);
        hardware.Write32(FakeXhci::OperationalOffset(X::Op::UsbCmd), X::UsbCmd::RunStop);
        hardware.SetEndpointRing(1, 3, transfers.Physical(), true);
    }

    static uint32_t Runtime(uint32_t reg)
    {
        return FakeXhci::RuntimeOffset(X::InterrupterRegister(0, reg));
    }

    std::vector<Trb> Drain()
    {
        std::vector<Trb> consumed;
        Trb trb;
        while (events.Next(&trb))
        {
            consumed.push_back(trb);
        }
        if (!consumed.empty())
        {
            hardware.Write64(Runtime(X::Runtime::Erdp), events.DequeuePointer());
        }
        return consumed;
    }

    Trb ReadSlot(const Buffer& buffer, uint32_t index) const
    {
        const auto* words = reinterpret_cast<const volatile uint32_t*>(buffer.address) + index * 4;
        return {words[0], words[1], words[2], words[3]};
    }

    PhysicalMemory memory;
    FakeXhci hardware{memory, 4, false};
    Buffer command_buffer{};
    Buffer transfer_buffer{};
    Buffer event_buffer{};
    X::ProducerRing commands;
    X::ProducerRing transfers;
    X::EventRing events;
};

std::vector<Trb> Td(uint32_t count, bool interrupt)
{
    std::vector<Trb> td(count, X::MakeTrb(X::TrbType::Normal, X::TrbControl::Chain, DataAddress, 8));
    td.back().control = X::TypeField(X::TrbType::Normal) |
                        (interrupt ? X::TrbControl::InterruptOnCompletion : 0);
    return td;
}

std::vector<Trb> Singles(uint32_t count)
{
    return std::vector<Trb>(count, X::MakeTrb(X::TrbType::Normal, 0, DataAddress, 8));
}

bool Valid(const Trb& trb, bool cycle)
{
    return ((trb.control & X::TrbControl::Cycle) != 0) == cycle;
}

struct Access
{
    const volatile uint32_t* address;
    uint32_t value;
};

std::vector<Access>* recorded = nullptr;

void Record(const volatile uint32_t* address, uint32_t value)
{
    recorded->push_back({address, value});
}

class Recording
{
public:
    Recording()
    {
        recorded = &log;
        X::Testing::SetObserver(Record);
    }
    Recording(const Recording&) = delete;
    Recording& operator=(const Recording&) = delete;
    ~Recording()
    {
        X::Testing::SetObserver(nullptr);
        recorded = nullptr;
    }

    std::vector<Access> log;
};

} // namespace

TEST_CASE("xhci ring: initialization needs a page-sized aligned buffer", "[xhci][ring]")
{
    PhysicalMemory memory;
    Buffer good = memory.Allocate(X::RingBytes);
    X::ProducerRing ring;
    X::EventRing events;

    Buffer small = good;
    small.size = X::RingBytes - 1;
    CHECK_FALSE(ring.Initialize(small));
    CHECK_FALSE(events.Initialize(small));
    Buffer unaligned = good;
    unaligned.physical += 16;
    CHECK_FALSE(ring.Initialize(unaligned));
    Buffer empty{};
    CHECK_FALSE(ring.Initialize(empty));

    CHECK(ring.Free() == 0);
    X::Trb trb = X::MakeTrb(X::TrbType::NoOp, 0, 0, 0);
    CHECK_FALSE(ring.Enqueue(std::span<const Trb>(&trb, 1), nullptr));
    REQUIRE(ring.Initialize(good));
    CHECK(ring.Free() == X::RingTrbs - 1);
    CHECK(ring.CycleState());
}

TEST_CASE("xhci ring: a TD that does not fit before the link is padded and the link wraps",
          "[xhci][ring]")
{
    Rig rig;
    uint64_t first = 0;
    for (uint32_t index = 0; index < 253; ++index)
    {
        REQUIRE(rig.transfers.Enqueue(Singles(1), &first));
    }
    REQUIRE(rig.transfers.Retire(first));
    const uint64_t base = rig.transfers.Physical();
    REQUIRE(first == base + 252 * X::TrbSize);

    CHECK(rig.transfers.CycleState());
    REQUIRE(rig.transfers.Enqueue(Td(3, true), &first));
    CHECK(first == base);
    CHECK_FALSE(rig.transfers.CycleState());

    for (uint32_t index : {253U, 254U})
    {
        const Trb pad = rig.ReadSlot(rig.transfer_buffer, index);
        CHECK(X::TrbTypeOf(pad) == X::TrbType::NoOp);
        CHECK(Valid(pad, true));
    }
    const Trb link = rig.ReadSlot(rig.transfer_buffer, X::LinkIndex);
    CHECK(X::TrbTypeOf(link) == X::TrbType::Link);
    CHECK(X::TrbParameter(link) == base);
    CHECK((link.control & X::TrbControl::ToggleCycle) != 0);
    CHECK((link.control & X::TrbControl::Chain) == 0);
    CHECK(Valid(link, true));
    for (uint32_t index = 0; index < 3; ++index)
    {
        const Trb trb = rig.ReadSlot(rig.transfer_buffer, index);
        CHECK(X::TrbTypeOf(trb) == X::TrbType::Normal);
        CHECK(Valid(trb, false));
        CHECK(((trb.control & X::TrbControl::Chain) != 0) == (index < 2));
    }
}

TEST_CASE("xhci ring: a TD that ends exactly at the link is not padded", "[xhci][ring]")
{
    Rig rig;
    uint64_t first = 0;
    REQUIRE(rig.transfers.Enqueue(Singles(252), &first));
    REQUIRE(rig.transfers.Enqueue(Td(3, true), &first));
    CHECK(first == rig.transfers.Physical() + 252 * X::TrbSize);
    CHECK(rig.transfers.CycleState());
    CHECK(X::TrbTypeOf(rig.ReadSlot(rig.transfer_buffer, 254)) == X::TrbType::Normal);
    CHECK(X::TrbTypeOf(rig.ReadSlot(rig.transfer_buffer, 255)) == 0);
}

TEST_CASE("xhci ring: hardware consumes TDs of every size over many laps", "[xhci][ring]")
{
    Rig rig;
    bool cycle = rig.transfers.CycleState();
    uint32_t toggles = 0;
    uint32_t enqueued = 0;
    uint32_t sequence = 0;
    for (uint32_t round = 0; round < 300; ++round)
    {
        std::vector<uint64_t> lasts;
        const uint32_t batch = 1 + round % 4;
        for (uint32_t index = 0; index < batch; ++index)
        {
            const uint32_t size = 1 + (sequence++ * 5) % 7;
            uint64_t first = 0;
            REQUIRE(rig.transfers.Enqueue(Td(size, true), &first));
            lasts.push_back(first + (size - 1) * X::TrbSize);
            enqueued += size;
        }
        rig.hardware.Write32(FakeXhci::DoorbellOffset(1), 3);
        const std::vector<Trb> events = rig.Drain();
        REQUIRE(events.size() == batch);
        for (uint32_t index = 0; index < batch; ++index)
        {
            CHECK(X::TrbTypeOf(events[index]) == X::TrbType::TransferEvent);
            CHECK(X::TrbParameter(events[index]) == lasts[index]);
            CHECK(events[index].status >> X::CompletionShift == X::CompletionCode::Success);
            REQUIRE(rig.transfers.Retire(X::TrbParameter(events[index])));
        }
        REQUIRE(rig.transfers.Free() == X::RingTrbs - 1);
        if (rig.transfers.CycleState() != cycle)
        {
            cycle = rig.transfers.CycleState();
            ++toggles;
        }
    }
    CHECK(toggles >= 3);
    CHECK(enqueued > 3 * X::RingTrbs);
    CHECK(rig.hardware.Faults() == 0);
    CHECK(rig.hardware.DroppedEvents() == 0);
    uint32_t normal = 0;
    for (uint32_t type : rig.hardware.TransferTypes())
    {
        normal += type == X::TrbType::Normal ? 1 : 0;
    }
    CHECK(normal == enqueued);
}

TEST_CASE("xhci ring: hardware keeps several TDs in flight across the link", "[xhci][ring]")
{
    Rig rig;
    std::vector<uint64_t> pending;
    uint32_t completed = 0;
    uint32_t issued = 0;
    while (completed < 400)
    {
        uint64_t first = 0;
        const uint32_t size = 2 + issued % 5;
        if (issued < 400 && pending.size() < 40 && rig.transfers.Enqueue(Td(size, true), &first))
        {
            pending.push_back(first + (size - 1) * X::TrbSize);
            ++issued;
            continue;
        }
        rig.hardware.Write32(FakeXhci::DoorbellOffset(1), 3);
        const std::vector<Trb> events = rig.Drain();
        REQUIRE_FALSE(events.empty());
        for (const Trb& event : events)
        {
            REQUIRE_FALSE(pending.empty());
            REQUIRE(X::TrbParameter(event) == pending.front());
            REQUIRE(rig.transfers.Retire(X::TrbParameter(event)));
            pending.erase(pending.begin());
            ++completed;
        }
    }
    CHECK(rig.transfers.Free() == X::RingTrbs - 1);
    CHECK(rig.hardware.Faults() == 0);
}

TEST_CASE("xhci ring: the command ring runs for several laps with its events", "[xhci][ring]")
{
    Rig rig;
    uint32_t completed = 0;
    for (uint32_t round = 0; round < 12; ++round)
    {
        std::vector<uint64_t> physical;
        for (uint32_t index = 0; index < 50; ++index)
        {
            uint64_t first = 0;
            const Trb command = X::MakeTrb(X::TrbType::NoOpCommand, 0, 0, 0);
            REQUIRE(rig.commands.Enqueue(std::span<const Trb>(&command, 1), &first));
            physical.push_back(first);
        }
        rig.hardware.Write32(FakeXhci::DoorbellOffset(0), 0);
        const std::vector<Trb> events = rig.Drain();
        REQUIRE(events.size() == physical.size());
        for (uint32_t index = 0; index < events.size(); ++index)
        {
            CHECK(X::TrbTypeOf(events[index]) == X::TrbType::CommandCompletionEvent);
            REQUIRE(X::TrbParameter(events[index]) == physical[index]);
            REQUIRE(rig.commands.Retire(physical[index]));
            ++completed;
        }
    }
    CHECK(completed == 600);
    CHECK(rig.commands.Free() == X::RingTrbs - 1);
    CHECK(rig.hardware.Faults() == 0);
    CHECK(rig.hardware.CommandTypes().size() == 600);
}

TEST_CASE("xhci ring: every TRB gets dwords 0 to 2, a barrier, then dword 3", "[xhci][ring]")
{
    Rig rig;
    Recording recording;
    uint64_t first = 0;
    uint32_t trbs = 0;
    for (uint32_t round = 0; round < 120; ++round)
    {
        const uint32_t size = 1 + round % 6;
        REQUIRE(rig.transfers.Enqueue(Td(size, true), &first));
        trbs += size;
        rig.hardware.Write32(FakeXhci::DoorbellOffset(1), 3);
        for (const Trb& event : rig.Drain())
        {
            REQUIRE(rig.transfers.Retire(X::TrbParameter(event)));
        }
    }
    const std::vector<Access>& log = recording.log;
    REQUIRE(log.size() % 5 == 0);
    REQUIRE(log.size() / 5 > trbs);
    for (size_t index = 0; index < log.size(); index += 5)
    {
        const volatile uint32_t* slot = log[index].address;
        const auto* base = reinterpret_cast<const volatile uint32_t*>(rig.transfer_buffer.address);
        REQUIRE(slot != nullptr);
        REQUIRE((slot - base) % 4 == 0);
        CHECK(log[index + 1].address == slot + 1);
        CHECK(log[index + 2].address == slot + 2);
        CHECK(log[index + 3].address == nullptr);
        CHECK(log[index + 4].address == slot + 3);
    }
}

TEST_CASE("xhci ring: a full ring refuses a TD and a retire gives the space back", "[xhci][ring]")
{
    Rig rig;
    std::vector<uint64_t> firsts;
    for (uint32_t index = 0; index < X::RingTrbs - 1; ++index)
    {
        uint64_t first = 0;
        REQUIRE(rig.transfers.Enqueue(Singles(1), &first));
        firsts.push_back(first);
    }
    CHECK(rig.transfers.Free() == 0);
    {
        Recording recording;
        uint64_t first = 0;
        CHECK_FALSE(rig.transfers.Enqueue(Singles(1), &first));
        CHECK(recording.log.empty());
    }

    REQUIRE(rig.transfers.Retire(firsts[0]));
    CHECK(rig.transfers.Free() == 1);
    uint64_t first = 0;
    CHECK_FALSE(rig.transfers.Enqueue(Singles(1), &first));
    REQUIRE(rig.transfers.Retire(firsts[1]));
    CHECK(rig.transfers.Free() == 2);
    REQUIRE(rig.transfers.Enqueue(Singles(1), &first));
    CHECK(first == rig.transfers.Physical());
    CHECK(rig.transfers.Free() == 0);

    REQUIRE(rig.transfers.Retire(firsts.back()));
    CHECK(rig.transfers.Free() == X::RingTrbs - 3);
    REQUIRE(rig.transfers.Retire(first));
    CHECK(rig.transfers.Free() == X::RingTrbs - 1);
}

TEST_CASE("xhci ring: a TD larger than the ring never fits and an empty one is refused",
          "[xhci][ring]")
{
    Rig rig;
    uint64_t first = 0;
    CHECK_FALSE(rig.transfers.Enqueue(Singles(X::RingTrbs), &first));
    CHECK_FALSE(rig.transfers.Enqueue({}, &first));
    CHECK(rig.transfers.Free() == X::RingTrbs - 1);
    REQUIRE(rig.transfers.Enqueue(Singles(X::RingTrbs - 1), &first));
    CHECK(first == rig.transfers.Physical());
    CHECK(rig.transfers.Free() == 0);
}

TEST_CASE("xhci ring: retiring unknown, stale or unsent TRBs changes nothing", "[xhci][ring]")
{
    Rig rig;
    const uint64_t base = rig.transfers.Physical();
    uint64_t first = 0;
    REQUIRE(rig.transfers.Enqueue(Singles(4), &first));
    CHECK(rig.transfers.InFlight(base + 3 * X::TrbSize));
    CHECK_FALSE(rig.transfers.InFlight(base + 4 * X::TrbSize));

    const uint64_t unknown[] = {0,
                                base - X::TrbSize,
                                base + X::RingBytes,
                                base + 4 * X::TrbSize,
                                base + 8,
                                base + X::LinkIndex * X::TrbSize,
                                rig.command_buffer.physical,
                                ~0ULL};
    for (const uint64_t pointer : unknown)
    {
        CHECK_FALSE(rig.transfers.Retire(pointer));
    }
    CHECK(rig.transfers.Free() == X::RingTrbs - 1 - 4);

    REQUIRE(rig.transfers.Retire(base + 1 * X::TrbSize));
    CHECK(rig.transfers.Free() == X::RingTrbs - 1 - 2);
    CHECK_FALSE(rig.transfers.Retire(base));
    CHECK_FALSE(rig.transfers.Retire(base + 1 * X::TrbSize));
    CHECK(rig.transfers.Free() == X::RingTrbs - 1 - 2);
}

TEST_CASE("xhci event ring: it stops at the first cycle mismatch and reports ERDP with EHB",
          "[xhci][event-ring]")
{
    Rig rig;
    const uint64_t base = rig.events.Physical();
    Trb event;
    CHECK_FALSE(rig.events.Next(&event));
    CHECK(rig.events.DequeuePointer() == (base | X::EventHandlerBusy));

    for (uint32_t port = 1; port <= 3; ++port)
    {
        rig.hardware.Connect(port, FeatherOS::Usb::Speed::High);
    }
    uint32_t count = 0;
    while (rig.events.Next(&event))
    {
        ++count;
        CHECK(X::TrbTypeOf(event) == X::TrbType::PortStatusChangeEvent);
        CHECK(X::TrbParameter(event) >> 24 == count);
    }
    CHECK(count == 3);
    CHECK_FALSE(rig.events.Next(&event));
    CHECK(rig.events.DequeuePointer() == (base + 3 * X::TrbSize + X::EventHandlerBusy));

    rig.hardware.Write64(Rig::Runtime(X::Runtime::Erdp), rig.events.DequeuePointer());
    REQUIRE(rig.hardware.EventPointerWrites().size() == 1);
    CHECK(rig.hardware.EventPointerWrites()[0] == rig.events.DequeuePointer());
    CHECK((rig.hardware.Read32(FakeXhci::RuntimeOffset(X::InterrupterRegister(0, X::Runtime::Erdp))) &
           X::EventHandlerBusy) == 0);
}

TEST_CASE("xhci event ring: the consumer cycle toggles at the segment end", "[xhci][event-ring]")
{
    Rig rig;
    const uint64_t base = rig.events.Physical();
    uint32_t seen = 0;
    uint32_t toggles = 0;
    bool cycle = rig.events.ConsumerCycle();
    for (uint32_t round = 0; round < 10; ++round)
    {
        for (uint32_t index = 0; index < 100; ++index)
        {
            rig.hardware.Connect(1 + index % 4, FeatherOS::Usb::Speed::Full);
        }
        Trb event;
        uint32_t consumed = 0;
        while (rig.events.Next(&event))
        {
            CHECK(X::TrbParameter(event) >> 24 == 1 + (seen % 100) % 4);
            ++seen;
            ++consumed;
        }
        REQUIRE(consumed == 100);
        rig.hardware.Write64(Rig::Runtime(X::Runtime::Erdp), rig.events.DequeuePointer());
        const uint64_t written = rig.hardware.EventPointerWrites().back();
        CHECK((written & X::EventHandlerBusy) != 0);
        CHECK(((written & ~X::EventHandlerBusy) - base) / X::TrbSize == seen % X::RingTrbs);
        if (rig.events.ConsumerCycle() != cycle)
        {
            cycle = rig.events.ConsumerCycle();
            ++toggles;
        }
    }
    CHECK(seen == 1000);
    CHECK(toggles == 3);
    CHECK(rig.hardware.DroppedEvents() == 0);
}

TEST_CASE("xhci event ring: the controller never overwrites events not yet consumed",
          "[xhci][event-ring]")
{
    Rig rig;
    for (uint32_t index = 0; index < 300; ++index)
    {
        rig.hardware.Connect(1 + index % 4, FeatherOS::Usb::Speed::Full);
    }
    CHECK(rig.hardware.DroppedEvents() == 300 - (X::RingTrbs - 1));
    Trb event;
    uint32_t consumed = 0;
    while (rig.events.Next(&event))
    {
        CHECK(X::TrbParameter(event) >> 24 == 1 + consumed % 4);
        ++consumed;
    }
    CHECK(consumed == X::RingTrbs - 1);
}
