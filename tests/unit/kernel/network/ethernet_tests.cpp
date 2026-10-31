#include <catch2/catch_test_macros.hpp>

#include "kernel/network/ethernet.h"

using namespace FeatherOS::Network;

namespace
{

constexpr MacAddress Local = {{0x52, 0x54, 0x00, 0x12, 0x34, 0x56}};

} // namespace

TEST_CASE("a short frame is padded to the Ethernet minimum")
{
    uint8_t frame[MaximumFrameSize];
    const uint8_t payload[] = {1, 2, 3};
    const size_t length =
        BuildEthernetFrame(frame, BroadcastMac, Local, EtherTypeArp, payload, sizeof(payload));
    REQUIRE(length == MinimumFrameSize);
    REQUIRE(frame[12] == 0x08);
    REQUIRE(frame[13] == 0x06);
    REQUIRE(frame[14] == 1);
    REQUIRE(frame[16] == 3);
    REQUIRE(frame[17] == 0);
    REQUIRE(frame[MinimumFrameSize - 1] == 0);

    EthernetFrame parsed;
    REQUIRE(ParseEthernetFrame(frame, length, &parsed));
    REQUIRE(parsed.destination == BroadcastMac);
    REQUIRE(parsed.source == Local);
    REQUIRE(parsed.type == EtherTypeArp);
    REQUIRE(parsed.payload == frame + EthernetHeaderSize);
    REQUIRE(parsed.payload_length == MinimumFrameSize - EthernetHeaderSize);
}

TEST_CASE("a full-size payload fits and a larger one does not")
{
    static uint8_t payload[MaximumPayloadSize + 1];
    uint8_t frame[MaximumFrameSize];
    REQUIRE(BuildEthernetFrame(frame, Local, Local, EtherTypeIpv4, payload, MaximumPayloadSize) ==
            MaximumFrameSize);
    REQUIRE(BuildEthernetFrame(frame, Local, Local, EtherTypeIpv4, payload,
                               MaximumPayloadSize + 1) == 0);
}

TEST_CASE("frames outside the Ethernet sizes are rejected")
{
    static uint8_t frame[MaximumFrameSize + 1];
    EthernetFrame parsed;
    REQUIRE_FALSE(ParseEthernetFrame(frame, EthernetHeaderSize - 1, &parsed));
    REQUIRE_FALSE(ParseEthernetFrame(frame, MaximumFrameSize + 1, &parsed));
    REQUIRE(ParseEthernetFrame(frame, EthernetHeaderSize, &parsed));
    REQUIRE(parsed.payload_length == 0);
}
