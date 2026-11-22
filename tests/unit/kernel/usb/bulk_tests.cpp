#include <catch2/catch_test_macros.hpp>

#include "fake_device.h"
#include "fake_xhci.h"
#include "rig.h"

#include "kernel/usb/xhci/controller.h"
#include "kernel/usb/xhci/ring.h"
#include "kernel/usb/xhci/td.h"
#include "kernel/usb/xhci/trb.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace
{

namespace X = FeatherOS::Usb::Xhci;
namespace T = FeatherOS::Usb;
namespace Dma = FeatherOS::Memory::Dma;
using FeatherOS::Usb::Speed;
using FeatherOS::Usb::Status;
using FeatherOS::Usb::Transfer;
using FeatherOS::Usb::Testing::FakeXhci;
using FeatherOS::Usb::Testing::MakeKeyboard;
using FeatherOS::Usb::Testing::MakeMassStorage;
using FeatherOS::Usb::Testing::Rig;
using FeatherOS::Usb::Testing::ScriptedDevice;
using Device = std::shared_ptr<ScriptedDevice>;
using Outcome = ScriptedDevice::Outcome;

constexpr uint8_t BulkIn = 0x81;
constexpr uint8_t BulkOut = 0x02;
constexpr uint32_t InDci = 3;
constexpr uint32_t OutDci = 4;
constexpr uint32_t Packet = 512;

std::vector<uint8_t> Pattern(size_t size, uint8_t seed)
{
    std::vector<uint8_t> bytes(size);
    for (size_t index = 0; index < size; ++index)
    {
        bytes[index] = static_cast<uint8_t>(seed + index * 7 + (index >> 8));
    }
    return bytes;
}

std::vector<uint32_t> CommandsSince(const Rig& rig, size_t from)
{
    const auto& all = rig.fake.CommandTypes();
    return std::vector<uint32_t>(all.begin() + static_cast<long>(from), all.end());
}

// A buffer inside one big allocation whose physical address has the given offset in its 64 KiB
// window, so a TD of it crosses the window boundary where the test wants it to.
Dma::Buffer Windowed(const Dma::Buffer& region, uint32_t offset)
{
    const uint64_t delta = (offset - (region.physical & 0xFFFF)) & 0xFFFF;
    return {region.physical + delta, region.address + delta, region.size - delta};
}

struct Io
{
    Transfer transfer;
    Dma::Buffer buffer{};
    int calls = 0;
};

void Counted(Transfer& transfer)
{
    ++static_cast<Io*>(transfer.context)->calls;
}

struct Storage
{
    Storage()
    {
        REQUIRE(rig.Create());
        rig.fake.Connect(1, Speed::High, true, device);
        rig.Settle();
        slot = rig.manager->SlotOfPort(1);
        REQUIRE(slot != 0);
        const T::Device* info = rig.manager->Find(slot);
        REQUIRE(info != nullptr);
        const T::InterfaceView& interface = info->configuration.interfaces[0];
        const std::vector<T::EndpointDescriptor> endpoints(
            interface.endpoints, interface.endpoints + interface.endpoint_count);
        REQUIRE(rig.controller->ConfigureEndpoints(slot, endpoints, nullptr));
        Dma::Buffer none{};
        uint32_t actual = 0;
        REQUIRE(rig.controller->Control(slot, {0x00, 9, info->configuration.value, 0, 0}, none,
                                        &actual) == Status::Success);
        REQUIRE(rig.controller->AllocateBuffer(3 * 65536, &region));
    }

    ~Storage()
    {
        rig.controller->FreeBuffer(region);
    }

    Storage(const Storage&) = delete;
    Storage& operator=(const Storage&) = delete;

    void Start(Io& io, uint8_t endpoint, uint32_t length, uint32_t window_offset = 0,
               bool callback = true)
    {
        io.buffer = Windowed(region, window_offset);
        io.transfer.endpoint = endpoint;
        io.transfer.buffer = &io.buffer;
        io.transfer.length = length;
        io.transfer.context = &io;
        io.transfer.done = callback ? Counted : nullptr;
    }

    bool Run(Io& io)
    {
        if (!rig.controller->Submit(slot, io.transfer))
        {
            return false;
        }
        rig.controller->ProcessEvents();
        return true;
    }

    void Fill(const Io& io, const std::vector<uint8_t>& bytes) const
    {
        for (size_t index = 0; index < bytes.size(); ++index)
        {
            io.buffer.address[index] = bytes[index];
        }
    }

    std::vector<uint8_t> Read(const Io& io, size_t count) const
    {
        return std::vector<uint8_t>(const_cast<const uint8_t*>(io.buffer.address),
                                    const_cast<const uint8_t*>(io.buffer.address) + count);
    }

    uint64_t RingOf(uint32_t dci) const
    {
        for (const auto& configured : rig.fake.Configured())
        {
            if (configured.dci == dci && configured.ring != 0)
            {
                return configured.ring;
            }
        }
        return 0;
    }

    Rig rig;
    Device device = MakeMassStorage(Speed::High);
    uint32_t slot = 0;
    Dma::Buffer region{};
};

TEST_CASE("bulk T1: a Normal TD stays inside 64 KiB windows with the right flags", "[bulk][xhci]")
{
    for (const uint32_t max_packet : {64U, 512U, 1024U})
    {
        for (const bool in : {false, true})
        {
            for (const uint32_t offset : {0U, 0x0FFFU, 0xFFFFU})
            {
                for (const uint32_t length : {0U, 1U, 512U, 4095U, 4096U, 65535U, 65536U})
                {
                    INFO("mps " << max_packet << " in " << in << " offset " << offset << " length "
                                << length);
                    const uint64_t physical = 0x1'0000'0000ULL + offset;
                    std::array<X::Trb, X::MaxTdTrbs> trbs{};
                    const uint32_t count = X::BuildNormalTd(physical, length, in, max_packet, trbs);
                    const uint32_t first = std::min<uint32_t>(length, 0x10000 - offset);
                    REQUIRE(count == (length == first ? 1U : 2U));
                    uint32_t sum = 0;
                    for (uint32_t index = 0; index < count; ++index)
                    {
                        const X::Trb& trb = trbs[index];
                        const bool last = index + 1 == count;
                        const uint32_t trb_length = trb.status & X::TrbLengthMask;
                        const uint64_t address = X::TrbParameter(trb);
                        REQUIRE(X::TrbTypeOf(trb) == X::TrbType::Normal);
                        REQUIRE(trb_length <= 0x10000);
                        REQUIRE(address == (length == 0 ? 0 : physical + sum));
                        REQUIRE((address & 0xFFFF) + trb_length <= 0x10000);
                        REQUIRE(((trb.control & X::TrbControl::Chain) != 0) == !last);
                        REQUIRE(((trb.control & X::TrbControl::InterruptOnCompletion) != 0) == last);
                        REQUIRE(((trb.control & X::TrbControl::InterruptOnShortPacket) != 0) == in);
                        REQUIRE((trb.control & X::TrbControl::Cycle) == 0);
                        sum += trb_length;
                        const uint32_t after = length - sum;
                        REQUIRE(((trb.status & X::TdSizeMask) >> X::TdSizeShift) ==
                                std::min<uint32_t>(31, (after + max_packet - 1) / max_packet));
                        REQUIRE(X::TdTrbLength(physical, length, index) == trb_length);
                        REQUIRE(X::TdBytesBefore(physical, length, index) == sum - trb_length);
                    }
                    REQUIRE(sum == length);
                    REQUIRE(((trbs[count - 1].status & X::TdSizeMask) >> X::TdSizeShift) == 0);
                    REQUIRE(X::TdTrbLength(physical, length, count) == 0);
                }
            }
        }
    }
}

TEST_CASE("bulk T1: a TD that is too long, has no packet size or has no room is refused",
          "[bulk][xhci]")
{
    std::array<X::Trb, X::MaxTdTrbs> trbs{};
    REQUIRE(X::BuildNormalTd(0x1000, 65537, true, 512, trbs) == 0);
    REQUIRE(X::BuildNormalTd(0x1000, 0x20000, false, 512, trbs) == 0);
    REQUIRE(X::BuildNormalTd(0x1000, 100, true, 0, trbs) == 0);
    REQUIRE(X::BuildNormalTd(0xF000, 8192, true, 512, std::span<X::Trb>(trbs).first(1)) == 0);
    REQUIRE(X::BuildNormalTd(0xF000, 8192, true, 512, std::span<X::Trb>(trbs).first(2)) == 2);
}

TEST_CASE("bulk T1: TD Size counts the packets left and stops at 31", "[bulk][xhci]")
{
    std::array<X::Trb, X::MaxTdTrbs> trbs{};
    // First chunk 1 byte, then 65535 bytes of 64-byte packets: 1024 packets, capped at 31.
    REQUIRE(X::BuildNormalTd(0xFFFF, 65536, false, 64, trbs) == 2);
    REQUIRE(((trbs[0].status & X::TdSizeMask) >> X::TdSizeShift) == 31);
    // 3 packets of 512 left after a first chunk of 4096.
    REQUIRE(X::BuildNormalTd(0xF000, 4096 + 1025, false, 512, trbs) == 2);
    REQUIRE(((trbs[0].status & X::TdSizeMask) >> X::TdSizeShift) == 3);
}

TEST_CASE("bulk: transfers up to 64 KiB cross windows, control keeps its 16 KiB limit",
          "[bulk][xhci]")
{
    Storage storage;
    Io out;
    storage.Start(out, BulkOut, 65536, 0xF000);
    REQUIRE(storage.Run(out));
    REQUIRE(out.transfer.finished.load());
    REQUIRE(out.transfer.status == Status::Success);
    REQUIRE(out.transfer.actual == 65536);
    REQUIRE(storage.device->BulkReceived(BulkOut).size() == 1);
    REQUIRE(storage.device->BulkReceived(BulkOut)[0].size() == 65536);

    Io too_long;
    storage.Start(too_long, BulkOut, 65537);
    REQUIRE_FALSE(storage.Run(too_long));
    REQUIRE(too_long.transfer.status == Status::ControllerError);

    Dma::Buffer big{};
    REQUIRE(storage.rig.controller->AllocateBuffer(32768, &big));
    uint32_t actual = 0;
    REQUIRE(storage.rig.controller->Control(storage.slot, {0x80, 6, 0x0200, 0, 16385}, big,
                                            &actual) == Status::ControllerError);
    storage.rig.controller->FreeBuffer(big);
    REQUIRE(storage.rig.fake.Faults() == 0);
}

TEST_CASE("bulk T2: two-TRB TDs over three ring laps are padded before the link", "[bulk][xhci]")
{
    Storage storage;
    const uint64_t ring = storage.RingOf(OutDci) & ~uint64_t{0xF};
    REQUIRE(ring != 0);
    std::vector<std::vector<uint8_t>> expected;
    for (uint32_t round = 0; round < 400; ++round)
    {
        Io io;
        storage.Start(io, BulkOut, 4096, 0xF800);
        const std::vector<uint8_t> data = Pattern(4096, static_cast<uint8_t>(round));
        storage.Fill(io, data);
        REQUIRE(storage.Run(io));
        REQUIRE(io.transfer.finished.load());
        REQUIRE(io.transfer.status == Status::Success);
        REQUIRE(io.transfer.actual == 4096);
        expected.push_back(data);
    }
    REQUIRE(storage.device->BulkReceived(BulkOut) == expected);
    REQUIRE(storage.rig.fake.Faults() == 0);

    const auto* trbs = reinterpret_cast<const volatile X::Trb*>(storage.rig.memory.Translate(
        ring, X::RingBytes));
    REQUIRE(trbs != nullptr);
    const uint32_t link_control = trbs[X::LinkIndex].control;
    REQUIRE(X::TrbTypeOf({0, 0, 0, link_control}) == X::TrbType::Link);
    REQUIRE((link_control & X::TrbControl::ToggleCycle) != 0);
    REQUIRE((link_control & X::TrbControl::Chain) == 0);
    // Two-TRB TDs fill 254 of the 255 slots, so the last slot of every lap is padding.
    REQUIRE(X::TrbTypeOf({0, 0, 0, trbs[X::LinkIndex - 1].control}) == X::TrbType::NoOp);
    for (uint32_t index = 0; index < X::LinkIndex - 1; index += 2)
    {
        const uint32_t first = trbs[index].control;
        const uint32_t second = trbs[index + 1].control;
        REQUIRE(X::TrbTypeOf({0, 0, 0, first}) == X::TrbType::Normal);
        REQUIRE((first & X::TrbControl::Chain) != 0);
        REQUIRE((second & X::TrbControl::Chain) == 0);
        REQUIRE((second & X::TrbControl::InterruptOnCompletion) != 0);
    }
}

TEST_CASE("bulk T3: the residue of a short IN TD counts the bytes of every TRB before it",
          "[bulk][xhci]")
{
    struct Case
    {
        uint32_t produced;
        Status status;
    };
    // The buffer is at 0xF000 in its window: TRB 0 holds 4096 bytes, TRB 1 the next 4096.
    for (const Case entry : {Case{1000, Status::ShortPacket}, Case{4096, Status::ShortPacket},
                             Case{5000, Status::ShortPacket}, Case{8191, Status::ShortPacket},
                             Case{8192, Status::Success}, Case{0, Status::ShortPacket}})
    {
        INFO("produced " << entry.produced);
        Storage storage;
        Io io;
        storage.Start(io, BulkIn, 8192, 0xF000);
        const std::vector<uint8_t> data = Pattern(entry.produced, 0x31);
        storage.device->QueueReport(BulkIn, data);
        REQUIRE(storage.Run(io));
        REQUIRE(io.transfer.finished.load());
        REQUIRE(io.calls == 1);
        REQUIRE(io.transfer.status == entry.status);
        REQUIRE(io.transfer.actual == entry.produced);
        REQUIRE(storage.Read(io, entry.produced) == data);
        REQUIRE(storage.rig.fake.Faults() == 0);
    }
}

TEST_CASE("bulk T3: a short single-TRB TD reports its length", "[bulk][xhci]")
{
    Storage storage;
    Io io;
    storage.Start(io, BulkIn, 512);
    storage.device->QueueReport(BulkIn, Pattern(13, 1));
    REQUIRE(storage.Run(io));
    REQUIRE(io.transfer.status == Status::ShortPacket);
    REQUIRE(io.transfer.actual == 13);
    Io full;
    storage.Start(full, BulkIn, 512);
    storage.device->QueueReport(BulkIn, Pattern(512, 2));
    REQUIRE(storage.Run(full));
    REQUIRE(full.transfer.status == Status::Success);
    REQUIRE(full.transfer.actual == 512);
}

TEST_CASE("bulk T4: a stalled bulk endpoint is reset and cleared before the transfer fails",
          "[bulk][xhci]")
{
    Storage storage;
    Rig& rig = storage.rig;
    // One packet first, so both data toggles are 1 and a missed reset would lose the next packet.
    Io first;
    storage.Start(first, BulkIn, 512);
    storage.device->QueueReport(BulkIn, Pattern(100, 1));
    REQUIRE(storage.Run(first));
    REQUIRE(first.transfer.status == Status::ShortPacket);

    Io io;
    storage.Start(io, BulkIn, 512);
    size_t commands_at_clear = 0;
    int finished_at_clear = -1;
    storage.device->on_request = [&](const ScriptedDevice::Request& request)
    {
        if (request.request == 1)
        {
            commands_at_clear = rig.fake.CommandTypes().size();
            finished_at_clear = io.calls;
        }
    };
    storage.device->FailEndpoint(BulkIn, Outcome::Stall, 1);
    const size_t before = rig.fake.CommandTypes().size();
    const size_t sets = rig.fake.DequeueSets().size();
    REQUIRE(storage.Run(io));
    REQUIRE(io.calls == 0);
    rig.Settle();
    REQUIRE(io.calls == 1);
    REQUIRE(io.transfer.status == Status::Stall);
    REQUIRE(CommandsSince(rig, before) ==
            std::vector<uint32_t>{X::TrbType::ResetEndpoint, X::TrbType::SetTrDequeue});
    REQUIRE(commands_at_clear == before + 2);
    REQUIRE(finished_at_clear == 0);
    REQUIRE(storage.device->Count(0x02, 1) == 1);
    REQUIRE(storage.device->Requests().back().index == BulkIn);
    REQUIRE(storage.device->Requests().back().value == 0);
    REQUIRE(rig.fake.DequeueSets().size() == sets + 1);

    Io next;
    storage.Start(next, BulkIn, 512);
    const std::vector<uint8_t> data = Pattern(300, 9);
    storage.device->QueueReport(BulkIn, data);
    REQUIRE(storage.Run(next));
    REQUIRE(next.transfer.status == Status::ShortPacket);
    REQUIRE(next.transfer.actual == 300);
    REQUIRE(storage.Read(next, 300) == data);
    REQUIRE(rig.fake.Faults() == 0);
}

TEST_CASE("bulk T5: a bulk transaction error is never replayed, an interrupt one still is",
          "[bulk][xhci]")
{
    Storage storage;
    Rig& rig = storage.rig;
    Io io;
    storage.Start(io, BulkOut, 1024);
    storage.Fill(io, Pattern(1024, 5));
    storage.device->FailEndpoint(BulkOut, Outcome::TransactionError, 1);
    const size_t before = rig.fake.CommandTypes().size();
    REQUIRE(storage.Run(io));
    for (int round = 0; round < 8 && io.calls == 0; ++round)
    {
        rig.Settle();
    }
    REQUIRE(io.calls == 1);
    REQUIRE(io.transfer.status == Status::TransactionError);
    REQUIRE(storage.device->BulkReceived(BulkOut).size() == 1);
    REQUIRE(CommandsSince(rig, before) ==
            std::vector<uint32_t>{X::TrbType::ResetEndpoint, X::TrbType::SetTrDequeue});
    REQUIRE(storage.device->Count(0x02, 1) == 1);
    rig.Settle();
    REQUIRE(storage.device->BulkReceived(BulkOut).size() == 1);

    Io again;
    storage.Start(again, BulkOut, 1024);
    storage.Fill(again, Pattern(1024, 6));
    REQUIRE(storage.Run(again));
    REQUIRE(again.transfer.status == Status::Success);
    REQUIRE(storage.device->BulkReceived(BulkOut).size() == 2);
    REQUIRE(storage.device->BulkReceived(BulkOut)[1] == Pattern(1024, 6));
    REQUIRE(rig.fake.Faults() == 0);
}

TEST_CASE("bulk T5: transaction errors on an interrupt endpoint keep their three retries",
          "[bulk][xhci]")
{
    Rig rig;
    REQUIRE(rig.Create());
    Device keyboard = MakeKeyboard(Speed::High);
    rig.fake.Connect(1, Speed::High, true, keyboard);
    rig.Settle();
    const uint32_t slot = rig.manager->SlotOfPort(1);
    REQUIRE(slot != 0);
    const T::Device* info = rig.manager->Find(slot);
    const T::InterfaceView& interface = info->configuration.interfaces[0];
    const std::vector<T::EndpointDescriptor> endpoints(interface.endpoints,
                                                       interface.endpoints + interface.endpoint_count);
    REQUIRE(rig.controller->ConfigureEndpoints(slot, endpoints, nullptr));
    Dma::Buffer buffer{};
    REQUIRE(rig.controller->AllocateBuffer(64, &buffer));
    Io io;
    io.buffer = buffer;
    io.transfer.endpoint = 0x81;
    io.transfer.buffer = &io.buffer;
    io.transfer.length = 8;
    io.transfer.context = &io;
    io.transfer.done = Counted;
    keyboard->FailEndpoint(0x81, Outcome::TransactionError, 100);
    const size_t before = rig.fake.CommandTypes().size();
    REQUIRE(rig.controller->Submit(slot, io.transfer));
    rig.controller->ProcessEvents();
    for (int round = 0; round < 8 && io.calls == 0; ++round)
    {
        rig.Settle();
    }
    REQUIRE(io.transfer.status == Status::TransactionError);
    std::vector<uint32_t> expected;
    for (int attempt = 0; attempt < 4; ++attempt)
    {
        expected.push_back(X::TrbType::ResetEndpoint);
        expected.push_back(X::TrbType::SetTrDequeue);
    }
    REQUIRE(CommandsSince(rig, before) == expected);
    rig.controller->FreeBuffer(buffer);
}

TEST_CASE("bulk T6: ClearHalt resets the controller's toggle as well as the device's",
          "[bulk][xhci]")
{
    for (const bool stopped : {false, true})
    {
        INFO("stopped " << stopped);
        Storage storage;
        Rig& rig = storage.rig;
        Io first;
        storage.Start(first, BulkOut, 512);
        storage.Fill(first, Pattern(512, 1));
        REQUIRE(storage.Run(first));
        REQUIRE(first.transfer.status == Status::Success);
        REQUIRE(storage.device->Toggle(BulkOut));
        REQUIRE(rig.fake.HostToggle(storage.slot, OutDci));
        if (stopped)
        {
            REQUIRE(rig.controller->CancelEndpoint(storage.slot, BulkOut));
            FakeXhci::State state;
            REQUIRE(rig.fake.EndpointState(storage.slot, OutDci, &state));
            REQUIRE(state == FakeXhci::State::Stopped);
        }

        size_t commands_at_clear = 0;
        storage.device->on_request = [&](const ScriptedDevice::Request& request)
        {
            if (request.request == 1)
            {
                commands_at_clear = rig.fake.CommandTypes().size();
            }
        };
        const size_t before = rig.fake.CommandTypes().size();
        REQUIRE(rig.controller->ClearHalt(storage.slot, BulkOut));
        REQUIRE(CommandsSince(rig, before) ==
                std::vector<uint32_t>{X::TrbType::StopEndpoint, X::TrbType::ConfigureEndpoint});
        REQUIRE(commands_at_clear == before + 2);
        REQUIRE(rig.fake.Reconfigured().size() == 1);
        const auto& reconfigured = rig.fake.Reconfigured()[0];
        REQUIRE(reconfigured.slot == storage.slot);
        REQUIRE(reconfigured.dci == OutDci);
        REQUIRE((reconfigured.drop_flags & (1U << OutDci)) != 0);
        REQUIRE((reconfigured.add_flags & (1U << OutDci)) != 0);
        // One TRB was queued, and the producer's cycle state is still 1.
        REQUIRE(reconfigured.pointer == ((storage.RingOf(OutDci) & ~uint64_t{0xF}) + X::TrbSize + 1));
        REQUIRE(storage.device->Count(0x02, 1) == 1);
        REQUIRE(storage.device->Requests().back().index == BulkOut);
        REQUIRE_FALSE(storage.device->Toggle(BulkOut));
        REQUIRE_FALSE(rig.fake.HostToggle(storage.slot, OutDci));

        // Both sides start at DATA0, so this packet is not dropped.
        Io io;
        storage.Start(io, BulkOut, 512);
        const std::vector<uint8_t> data = Pattern(512, 77);
        storage.Fill(io, data);
        REQUIRE(storage.Run(io));
        REQUIRE(io.transfer.status == Status::Success);
        REQUIRE(storage.device->BulkReceived(BulkOut).size() == 2);
        REQUIRE(storage.device->BulkReceived(BulkOut)[1] == data);
        REQUIRE(rig.fake.Faults() == 0);
    }
}

TEST_CASE("bulk T6: ClearHalt on a halted endpoint resets it and cancels what failed",
          "[bulk][xhci]")
{
    Storage storage;
    Rig& rig = storage.rig;
    Io first;
    storage.Start(first, BulkIn, 512);
    storage.device->QueueReport(BulkIn, Pattern(100, 1));
    REQUIRE(storage.Run(first));
    REQUIRE(storage.device->Toggle(BulkIn));

    Io failed;
    storage.Start(failed, BulkIn, 512);
    storage.device->FailEndpoint(BulkIn, Outcome::Stall, 1);
    REQUIRE(storage.Run(failed));
    REQUIRE(failed.calls == 0);
    FakeXhci::State state;
    REQUIRE(rig.fake.EndpointState(storage.slot, InDci, &state));
    REQUIRE(state == FakeXhci::State::Halted);

    const size_t before = rig.fake.CommandTypes().size();
    REQUIRE(rig.controller->ClearHalt(storage.slot, BulkIn));
    REQUIRE(CommandsSince(rig, before) ==
            std::vector<uint32_t>{X::TrbType::ResetEndpoint, X::TrbType::SetTrDequeue});
    REQUIRE(rig.fake.Reconfigured().empty());
    REQUIRE(failed.calls == 1);
    REQUIRE(failed.transfer.status == Status::Cancelled);
    REQUIRE(storage.device->Count(0x02, 1) == 1);
    REQUIRE_FALSE(storage.device->Toggle(BulkIn));
    rig.Settle();
    REQUIRE(failed.calls == 1);

    Io next;
    storage.Start(next, BulkIn, 512);
    const std::vector<uint8_t> data = Pattern(200, 4);
    storage.device->QueueReport(BulkIn, data);
    REQUIRE(storage.Run(next));
    REQUIRE(next.transfer.status == Status::ShortPacket);
    REQUIRE(next.transfer.actual == 200);
    REQUIRE(storage.Read(next, 200) == data);
    REQUIRE(rig.fake.Faults() == 0);
}

TEST_CASE("bulk T6: ClearHalt cancels what is pending on the endpoint", "[bulk][xhci]")
{
    Storage storage;
    Io pending;
    storage.Start(pending, BulkIn, 512);
    REQUIRE(storage.Run(pending));
    REQUIRE(pending.calls == 0);
    REQUIRE(storage.rig.controller->ClearHalt(storage.slot, BulkIn));
    REQUIRE(pending.calls == 1);
    REQUIRE(pending.transfer.status == Status::Cancelled);
    REQUIRE_FALSE(storage.rig.controller->ClearHalt(storage.slot, 0x03));
    REQUIRE_FALSE(storage.rig.controller->ClearHalt(storage.slot, 0x01));
}

TEST_CASE("bulk T7: CancelEndpoint finishes a pending IN and nothing is written afterwards",
          "[bulk][xhci]")
{
    Storage storage;
    Rig& rig = storage.rig;
    Io io;
    storage.Start(io, BulkIn, 512);
    const std::vector<uint8_t> sentinel(512, 0xAA);
    storage.Fill(io, sentinel);
    REQUIRE(storage.Run(io));
    REQUIRE(io.calls == 0);
    REQUIRE_FALSE(io.transfer.finished.load());

    const size_t before = rig.fake.CommandTypes().size();
    REQUIRE(rig.controller->CancelEndpoint(storage.slot, BulkIn));
    REQUIRE(io.transfer.finished.load());
    REQUIRE(io.calls == 1);
    REQUIRE(io.transfer.status == Status::Cancelled);
    const std::vector<uint32_t> commands = CommandsSince(rig, before);

    const size_t writes = rig.fake.DmaWrites().size();
    const std::vector<uint8_t> data = Pattern(100, 3);
    rig.fake.DeliverReport(storage.device, BulkIn, data);
    rig.fake.Wake(storage.device);
    rig.controller->ProcessEvents();
    rig.Settle();
    REQUIRE(rig.fake.DmaWrites().size() == writes);
    REQUIRE(storage.Read(io, 512) == sentinel);
    REQUIRE(io.calls == 1);

    // The endpoint takes new transfers, and the report that waited reaches the next one.
    Io next;
    storage.Start(next, BulkIn, 512);
    REQUIRE(storage.Run(next));
    REQUIRE(next.transfer.status == Status::ShortPacket);
    REQUIRE(next.transfer.actual == 100);
    REQUIRE(storage.Read(next, 100) == data);
    REQUIRE(rig.fake.Faults() == 0);
    REQUIRE(commands ==
            std::vector<uint32_t>{X::TrbType::StopEndpoint, X::TrbType::SetTrDequeue});
}

TEST_CASE("bulk T7: CancelEndpoint refuses an endpoint that is not configured", "[bulk][xhci]")
{
    Storage storage;
    REQUIRE_FALSE(storage.rig.controller->CancelEndpoint(storage.slot, 0x05));
    REQUIRE_FALSE(storage.rig.controller->CancelEndpoint(storage.slot, 0x00));
    REQUIRE_FALSE(storage.rig.controller->CancelEndpoint(0, BulkIn));
    REQUIRE_FALSE(storage.rig.controller->CancelEndpoint(99, BulkIn));
}

TEST_CASE("bulk: a TD whose data arrives later waits and completes", "[bulk][xhci]")
{
    Storage storage;
    Io io;
    storage.Start(io, BulkIn, 8192, 0xF000);
    REQUIRE(storage.Run(io));
    REQUIRE(io.calls == 0);
    const std::vector<uint8_t> data = Pattern(6000, 8);
    storage.rig.fake.DeliverReport(storage.device, BulkIn, data);
    storage.rig.controller->ProcessEvents();
    REQUIRE(io.calls == 1);
    REQUIRE(io.transfer.status == Status::ShortPacket);
    REQUIRE(io.transfer.actual == 6000);
    REQUIRE(storage.Read(io, 6000) == data);
}

TEST_CASE("bulk: a stall in the second TRB of a TD skips the whole TD", "[bulk][xhci]")
{
    Storage storage;
    Rig& rig = storage.rig;
    Io io;
    storage.Start(io, BulkOut, 8192, 0xF000);
    storage.device->FailEndpoint(BulkOut, Outcome::Stall, 1);
    REQUIRE(storage.Run(io));
    rig.Settle();
    REQUIRE(io.transfer.status == Status::Stall);
    // The dequeue pointer moved past both TRBs.
    REQUIRE(rig.fake.DequeueSets().back().pointer ==
            ((storage.RingOf(OutDci) & ~uint64_t{0xF}) + 2 * X::TrbSize + 1));
    Io next;
    storage.Start(next, BulkOut, 8192, 0xF000);
    storage.Fill(next, Pattern(8192, 2));
    REQUIRE(storage.Run(next));
    REQUIRE(next.transfer.status == Status::Success);
    REQUIRE(storage.device->BulkReceived(BulkOut).size() == 1);
    REQUIRE(storage.device->BulkReceived(BulkOut)[0] == Pattern(8192, 2));
}

} // namespace
