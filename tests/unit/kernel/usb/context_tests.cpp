#include <catch2/catch_test_macros.hpp>

#include "fake_xhci.h"

#include "kernel/usb/xhci/context.h"
#include "kernel/usb/xhci/registers.h"

#include <cstdint>
#include <initializer_list>
#include <map>
#include <vector>

namespace
{

namespace X = FeatherOS::Usb::Xhci;
using FeatherOS::Usb::EndpointDescriptor;
using FeatherOS::Usb::EndpointType;
using FeatherOS::Usb::HubInfo;
using FeatherOS::Usb::Location;
using FeatherOS::Usb::Speed;
using FeatherOS::Usb::Testing::FakeXhci;
using FeatherOS::Usb::Testing::PhysicalMemory;

constexpr uint32_t Garbage = 0xA5A5A5A5;

class Input
{
public:
    explicit Input(bool csz64) : layout(csz64), words(layout.InputBytes() / 4, 0)
    {
    }

    volatile uint8_t* Bytes()
    {
        return reinterpret_cast<volatile uint8_t*>(words.data());
    }

    void Dirty(uint32_t offset)
    {
        for (uint32_t index = 0; index < layout.ContextSize() / 4; ++index)
        {
            words[offset / 4 + index] = Garbage;
        }
    }

    void Expect(const std::map<uint32_t, uint32_t>& expected) const
    {
        for (uint32_t index = 0; index < words.size(); ++index)
        {
            const auto found = expected.find(index * 4);
            const uint32_t want = found == expected.end() ? 0 : found->second;
            INFO("offset " << index * 4);
            CHECK(words[index] == want);
        }
    }

    X::ContextLayout layout;
    std::vector<uint32_t> words;
};

constexpr uint64_t Ring = 0x1'2345'6000ULL;

const EndpointDescriptor ControlEndpoint{0, EndpointType::Control, 64, 0, 0, 0, 0};

} // namespace

TEST_CASE("xhci context: offsets follow the 32- or 64-byte context size", "[xhci][context]")
{
    for (const bool csz64 : {false, true})
    {
        const X::ContextLayout layout(csz64);
        const uint32_t size = csz64 ? 64 : 32;
        CHECK(layout.ContextSize() == size);
        CHECK(layout.ControlOffset() == 0);
        CHECK(layout.SlotOffset(true) == size);
        CHECK(layout.SlotOffset(false) == 0);
        CHECK(layout.EndpointOffset(true, 1) == 2 * size);
        CHECK(layout.EndpointOffset(true, 31) == 32 * size);
        CHECK(layout.EndpointOffset(false, 1) == size);
        CHECK(layout.EndpointOffset(false, 31) == 31 * size);
        CHECK(layout.InputBytes() == 33 * size);
        CHECK(layout.DeviceBytes() == 32 * size);
        CHECK(layout.InputBytes() <= 4096);
    }
}

TEST_CASE("xhci context: slot contexts of root devices, hubs and devices behind hubs",
          "[xhci][context]")
{
    struct Case
    {
        Location location;
        uint32_t entries;
        HubInfo hub;
        bool is_hub;
        std::map<uint32_t, uint32_t> words;
    };
    const Case cases[] = {
        {{Speed::High, 2, 0, 0, 0, 0, false},
         1,
         {},
         false,
         {{0, (3U << 20) | (1U << 27)}, {1, 2U << 16}, {2, 0}}},
        {{Speed::High, 1, 0, 0, 0, 0, false},
         1,
         {4, true, 1},
         true,
         {{0, (3U << 20) | (1U << 25) | (1U << 26) | (1U << 27)},
          {1, (1U << 16) | (4U << 24)},
          {2, 1U << 16}}},
        {{Speed::Full, 1, 1, 3, 0x3, 5, true},
         3,
         {},
         false,
         {{0, 0x3U | (1U << 20) | (1U << 25) | (3U << 27)}, {1, 1U << 16}, {2, 5U | (3U << 8)}}},
        {{Speed::Low, 4, 3, 4, 0x213, 7, false},
         1,
         {},
         false,
         {{0, 0x213U | (2U << 20) | (1U << 27)}, {1, 4U << 16}, {2, 7U | (4U << 8)}}},
        {{Speed::Super, 3, 0, 0, 0, 0, false},
         1,
         {4, false, 0},
         true,
         {{0, (4U << 20) | (1U << 26) | (1U << 27)}, {1, (3U << 16) | (4U << 24)}, {2, 0}}},
        {{Speed::SuperPlus, 1, 0, 0, 0xFFF12345, 0, false},
         31,
         {},
         false,
         {{0, 0x12345U | (5U << 20) | (31U << 27)}, {1, 1U << 16}, {2, 0}}},
    };
    for (const bool csz64 : {false, true})
    {
        for (const Case& test : cases)
        {
            Input input(csz64);
            const uint32_t slot = input.layout.SlotOffset(true);
            input.Dirty(slot);
            X::WriteSlot(input.layout, input.Bytes(), test.location, test.entries,
                         test.is_hub ? &test.hub : nullptr);
            std::map<uint32_t, uint32_t> expected;
            for (const auto& [word, value] : test.words)
            {
                expected[slot + word * 4] = value;
            }
            expected[X::ControlAddFlags] = 1;
            input.Expect(expected);
        }
    }
}

TEST_CASE("xhci context: endpoint contexts for every transfer type and speed", "[xhci][context]")
{
    struct Case
    {
        EndpointDescriptor endpoint;
        Speed speed;
        uint32_t dci;
        std::map<uint32_t, uint32_t> words;
    };
    const uint32_t ring_low = 0x23456001;
    const Case cases[] = {
        {ControlEndpoint,
         Speed::High,
         1,
         {{0, 0}, {1, (3U << 1) | (4U << 3) | (64U << 16)}, {2, ring_low}, {3, 1}, {4, 8}}},
        {{0, EndpointType::Control, 8, 0, 0, 0, 0},
         Speed::Full,
         1,
         {{0, 0}, {1, (3U << 1) | (4U << 3) | (8U << 16)}, {2, ring_low}, {3, 1}, {4, 8}}},
        {{0, EndpointType::Control, 512, 0, 0, 0, 0},
         Speed::Super,
         1,
         {{0, 0}, {1, (3U << 1) | (4U << 3) | (512U << 16)}, {2, ring_low}, {3, 1}, {4, 8}}},
        {{0x81, EndpointType::Interrupt, 8, 4, 0, 0, 0},
         Speed::High,
         3,
         {{0, 3U << 16},
          {1, (3U << 1) | (7U << 3) | (8U << 16)},
          {2, ring_low},
          {3, 1},
          {4, 8U | (8U << 16)}}},
        {{0x82, EndpointType::Interrupt, 8, 10, 0, 0, 0},
         Speed::Full,
         5,
         {{0, 6U << 16},
          {1, (3U << 1) | (7U << 3) | (8U << 16)},
          {2, ring_low},
          {3, 1},
          {4, 8U | (8U << 16)}}},
        {{0x81, EndpointType::Interrupt, 8, 10, 0, 0, 0},
         Speed::Low,
         3,
         {{0, 6U << 16},
          {1, (3U << 1) | (7U << 3) | (8U << 16)},
          {2, ring_low},
          {3, 1},
          {4, 8U | (8U << 16)}}},
        {{0x03, EndpointType::Interrupt, (2U << 11) | 1024, 1, 0, 0, 0},
         Speed::High,
         6,
         {{0, 0},
          {1, (3U << 1) | (3U << 3) | (2U << 8) | (1024U << 16)},
          {2, ring_low},
          {3, 1},
          {4, 3072U | (3072U << 16)}}},
        {{0x81, EndpointType::Interrupt, 1024, 4, 2, 0, 3072},
         Speed::Super,
         3,
         {{0, 3U << 16},
          {1, (3U << 1) | (7U << 3) | (2U << 8) | (1024U << 16)},
          {2, ring_low},
          {3, 1},
          {4, 3072U | (3072U << 16)}}},
        {{0x02, EndpointType::Bulk, 1024, 0, 15, 0, 0},
         Speed::Super,
         4,
         {{0, 0},
          {1, (3U << 1) | (2U << 3) | (15U << 8) | (1024U << 16)},
          {2, ring_low},
          {3, 1},
          {4, 3072}}},
        {{0x86, EndpointType::Bulk, 512, 0, 0, 0, 0},
         Speed::High,
         13,
         {{0, 0},
          {1, (3U << 1) | (6U << 3) | (512U << 16)},
          {2, ring_low},
          {3, 1},
          {4, 3072}}},
        {{0x85, EndpointType::Isochronous, 1024, 1, 15, 2, 0x1A000},
         Speed::Super,
         11,
         {{0, (2U << 8) | (1U << 24)},
          {1, (5U << 3) | (15U << 8) | (1024U << 16)},
          {2, ring_low},
          {3, 1},
          {4, 0xFFFFU | (0xA000U << 16)}}},
        {{0x8F, EndpointType::Interrupt, 64, 1, 0, 0, 0},
         Speed::High,
         31,
         {{0, 0},
          {1, (3U << 1) | (7U << 3) | (64U << 16)},
          {2, ring_low},
          {3, 1},
          {4, 64U | (64U << 16)}}},
    };
    for (const bool csz64 : {false, true})
    {
        for (const Case& test : cases)
        {
            INFO("endpoint " << static_cast<int>(test.endpoint.address) << " csz64 " << csz64);
            Input input(csz64);
            const uint32_t offset = input.layout.EndpointOffset(true, test.dci);
            input.Dirty(offset);
            X::WriteEndpoint(input.layout, input.Bytes(), test.endpoint, test.speed, Ring | 1);
            std::map<uint32_t, uint32_t> expected;
            for (const auto& [word, value] : test.words)
            {
                expected[offset + word * 4] = value;
            }
            expected[X::ControlAddFlags] = 1U << test.dci;
            input.Expect(expected);
        }
    }
}

TEST_CASE("xhci context: the slot and endpoints accumulate add flags", "[xhci][context]")
{
    Input input(true);
    const Location location{Speed::High, 1, 0, 0, 0, 0, false};
    X::WriteSlot(input.layout, input.Bytes(), location, 3, nullptr);
    X::WriteEndpoint(input.layout, input.Bytes(), ControlEndpoint, Speed::High, Ring | 1);
    const EndpointDescriptor interrupt{0x81, EndpointType::Interrupt, 8, 4, 0, 0, 0};
    X::WriteEndpoint(input.layout, input.Bytes(), interrupt, Speed::High, Ring | 1);
    CHECK(input.words[X::ControlAddFlags / 4] == 0b1011);
    CHECK(input.words[X::ControlDropFlags / 4] == 0);
}

TEST_CASE("xhci context: endpoint numbers map to device context indexes", "[xhci][context]")
{
    const auto index = [](uint8_t address) {
        return X::EndpointContextIndex({address, EndpointType::Interrupt, 8, 1, 0, 0, 0});
    };
    CHECK(index(0x00) == 1);
    CHECK(index(0x80) == 1);
    CHECK(index(0x01) == 2);
    CHECK(index(0x81) == 3);
    CHECK(index(0x0F) == 30);
    CHECK(index(0x8F) == 31);
}

TEST_CASE("xhci context: speeds use the controller's speed IDs, not the USB enum order",
          "[xhci][context]")
{
    CHECK(X::SpeedId(Speed::Full) == 1);
    CHECK(X::SpeedId(Speed::Low) == 2);
    CHECK(X::SpeedId(Speed::High) == 3);
    CHECK(X::SpeedId(Speed::Super) == 4);
    CHECK(X::SpeedId(Speed::SuperPlus) == 5);
    for (const Speed speed : {Speed::Low, Speed::Full, Speed::High, Speed::Super, Speed::SuperPlus})
    {
        Speed back = Speed::Low;
        REQUIRE(X::SpeedFromId(X::SpeedId(speed), &back));
        CHECK(back == speed);
    }
    Speed unused = Speed::Low;
    CHECK_FALSE(X::SpeedFromId(0, &unused));
    CHECK_FALSE(X::SpeedFromId(6, &unused));
}

TEST_CASE("xhci interval: exponents per speed for bInterval 1, 8, 10 and 255", "[xhci][interval]")
{
    const uint8_t intervals[] = {1, 8, 10, 255};
    const uint8_t slow[] = {3, 6, 6, 10};
    const uint8_t fast[] = {0, 7, 9, 15};
    for (uint32_t index = 0; index < 4; ++index)
    {
        INFO("bInterval " << static_cast<int>(intervals[index]));
        CHECK(X::IntervalExponent(Speed::Low, EndpointType::Interrupt, intervals[index]) ==
              slow[index]);
        CHECK(X::IntervalExponent(Speed::Full, EndpointType::Interrupt, intervals[index]) ==
              slow[index]);
        CHECK(X::IntervalExponent(Speed::High, EndpointType::Interrupt, intervals[index]) ==
              fast[index]);
        CHECK(X::IntervalExponent(Speed::Super, EndpointType::Interrupt, intervals[index]) ==
              fast[index]);
        CHECK(X::IntervalExponent(Speed::SuperPlus, EndpointType::Interrupt, intervals[index]) ==
              fast[index]);
    }
}

TEST_CASE("xhci interval: zero, other transfer types and isochronous", "[xhci][interval]")
{
    CHECK(X::IntervalExponent(Speed::Low, EndpointType::Interrupt, 0) == 3);
    CHECK(X::IntervalExponent(Speed::High, EndpointType::Interrupt, 0) == 0);
    CHECK(X::IntervalExponent(Speed::High, EndpointType::Bulk, 9) == 0);
    CHECK(X::IntervalExponent(Speed::Full, EndpointType::Control, 9) == 0);
    CHECK(X::IntervalExponent(Speed::Full, EndpointType::Isochronous, 1) == 3);
    CHECK(X::IntervalExponent(Speed::Full, EndpointType::Isochronous, 4) == 6);
    CHECK(X::IntervalExponent(Speed::Full, EndpointType::Isochronous, 255) == 10);
    CHECK(X::IntervalExponent(Speed::High, EndpointType::Isochronous, 4) == 3);
}

TEST_CASE("xhci PORTSC: a write keeps only the RW bits and the change bit being cleared",
          "[xhci][portsc]")
{
    namespace P = X::PortSc;
    const uint32_t rw = P::Power | P::IndicatorMask | P::LinkWriteStrobe | P::WakeOnConnect |
                        P::WakeOnDisconnect | P::WakeOnOverCurrent;
    const uint32_t everything = 0xFFFFFFFFU;
    const uint32_t changes[] = {P::ConnectChange,   P::EnableChange,    P::WarmResetChange,
                                P::OverCurrentChange, P::ResetChange,    P::LinkStateChange,
                                P::ConfigErrorChange};
    for (const uint32_t change : changes)
    {
        const uint32_t value = X::PortscWriteValue(everything, change);
        CHECK(value == (rw | change));
        CHECK((value & P::Enabled) == 0);
        CHECK((value & P::Reset) == 0);
        CHECK((value & P::WarmReset) == 0);
        CHECK((value & P::ChangeMask) == change);
    }
    CHECK(X::PortscWriteValue(everything, 0) == rw);
    CHECK(X::PortscWriteValue(0, P::ConnectChange) == P::ConnectChange);
    CHECK(X::PortscWriteValue(P::Power | P::CurrentConnect | P::Enabled, 0) == P::Power);
    CHECK(X::PortscWriteValue(0, everything) == P::ChangeMask);
    CHECK(X::PortscWriteValue(0, P::Enabled | P::Reset | P::WarmReset | P::ConnectChange) ==
          P::ConnectChange);
}

TEST_CASE("xhci PORTSC: clearing one change bit leaves the port enabled and the other changes",
          "[xhci][portsc]")
{
    namespace P = X::PortSc;
    PhysicalMemory memory;
    FakeXhci hardware(memory, 2, false);
    const uint32_t reg = FakeXhci::OperationalOffset(X::PortRegister(2, X::Op::PortSc));
    hardware.Connect(2, Speed::High);
    CHECK((hardware.Read32(reg) & (P::CurrentConnect | P::ConnectChange | P::Power)) ==
          (P::CurrentConnect | P::ConnectChange | P::Power));

    hardware.Write32(reg, X::PortscWriteValue(hardware.Read32(reg), 0) | P::Reset);
    uint32_t status = hardware.Read32(reg);
    CHECK((status & P::Enabled) != 0);
    CHECK((status & P::ResetChange) != 0);
    CHECK((status & P::ConnectChange) != 0);

    hardware.Write32(reg, X::PortscWriteValue(status, P::ResetChange));
    status = hardware.Read32(reg);
    CHECK((status & P::Enabled) != 0);
    CHECK((status & P::ResetChange) == 0);
    CHECK((status & P::ConnectChange) != 0);
    CHECK((status & P::Power) != 0);

    hardware.Write32(reg, X::PortscWriteValue(status, P::ConnectChange));
    status = hardware.Read32(reg);
    CHECK((status & P::Enabled) != 0);
    CHECK((status & P::ChangeMask) == 0);
    CHECK(hardware.PortEnableClears(2) == 0);
}

TEST_CASE("xhci PORTSC: writing the register back unmasked disables the port", "[xhci][portsc]")
{
    namespace P = X::PortSc;
    PhysicalMemory memory;
    FakeXhci hardware(memory, 1, false);
    const uint32_t reg = FakeXhci::OperationalOffset(X::PortRegister(1, X::Op::PortSc));
    hardware.Connect(1, Speed::Full);
    hardware.Write32(reg, X::PortscWriteValue(hardware.Read32(reg), 0) | P::Reset);
    REQUIRE((hardware.Read32(reg) & P::Enabled) != 0);
    hardware.Write32(reg, hardware.Read32(reg));
    CHECK((hardware.Read32(reg) & P::Enabled) == 0);
    CHECK(hardware.PortEnableClears(1) == 1);
}
