#include <catch2/catch_test_macros.hpp>

#include "kernel/network/address.h"
#include "kernel/network/tcp.h"

#include <vector>

using namespace FeatherOS::Network;

namespace
{

TcpSegment Fields(uint8_t flags, uint16_t mss = 0)
{
    TcpSegment fields{};
    fields.source_port = 49152;
    fields.destination_port = 80;
    fields.sequence = 0x01020304;
    fields.acknowledgment = 0x0A0B0C0D;
    fields.flags = flags;
    fields.window = 0x4000;
    fields.maximum_segment_size = mss;
    return fields;
}

} // namespace

TEST_CASE("a TCP header is written in front of its payload")
{
    const uint8_t payload[] = {'h', 'i'};
    TcpSegment fields = Fields(TcpAck | TcpPush);
    fields.payload = payload;
    fields.payload_length = sizeof(payload);
    std::vector<uint8_t> data(64, 0xEE);
    REQUIRE(WriteTcpSegment(data.data(), data.size(), fields) == TcpHeaderSize + 2);
    REQUIRE(std::vector<uint8_t>(data.begin(), data.begin() + 22) ==
            std::vector<uint8_t>{0xC0, 0x00, 0x00, 0x50, 0x01, 0x02, 0x03, 0x04, 0x0A, 0x0B, 0x0C,
                                 0x0D, 0x50, 0x18, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 'h', 'i'});

    TcpSegment parsed{};
    REQUIRE(ParseTcpSegment(data.data(), TcpHeaderSize + 2, &parsed));
    REQUIRE(parsed.source_port == 49152);
    REQUIRE(parsed.destination_port == 80);
    REQUIRE(parsed.sequence == 0x01020304);
    REQUIRE(parsed.acknowledgment == 0x0A0B0C0D);
    REQUIRE(parsed.flags == (TcpAck | TcpPush));
    REQUIRE(parsed.window == 0x4000);
    REQUIRE(parsed.maximum_segment_size == 0);
    REQUIRE(parsed.payload == data.data() + TcpHeaderSize);
    REQUIRE(parsed.payload_length == 2);
    REQUIRE(SequenceLength(parsed) == 2);
}

TEST_CASE("a SYN carries the MSS option")
{
    std::vector<uint8_t> data(TcpHeaderSize + 4);
    REQUIRE(WriteTcpSegment(data.data(), data.size(), Fields(TcpSyn, 1460)) == data.size());
    REQUIRE(data[12] == 0x60);
    REQUIRE(std::vector<uint8_t>(data.begin() + 20, data.end()) ==
            std::vector<uint8_t>{2, 4, 0x05, 0xB4});
    TcpSegment parsed{};
    REQUIRE(ParseTcpSegment(data.data(), data.size(), &parsed));
    REQUIRE(parsed.maximum_segment_size == 1460);
    REQUIRE_FALSE(parsed.has_window_scale);
    REQUIRE(parsed.payload_length == 0);
    REQUIRE(SequenceLength(parsed) == 1);
}

TEST_CASE("a SYN carries the window scale option after the MSS")
{
    TcpSegment fields = Fields(TcpSyn, 1460);
    fields.has_window_scale = true;
    fields.window_scale = 3;
    std::vector<uint8_t> data(TcpHeaderSize + TcpMaximumOptionsSize);
    REQUIRE(WriteTcpSegment(data.data(), data.size(), fields) == data.size());
    REQUIRE(data[12] == 0x70);
    REQUIRE(std::vector<uint8_t>(data.begin() + 20, data.end()) ==
            std::vector<uint8_t>{2, 4, 0x05, 0xB4, 1, 3, 3, 3});
    TcpSegment parsed{};
    REQUIRE(ParseTcpSegment(data.data(), data.size(), &parsed));
    REQUIRE(parsed.maximum_segment_size == 1460);
    REQUIRE(parsed.has_window_scale);
    REQUIRE(parsed.window_scale == 3);
}

TEST_CASE("a window scale above 14 counts as 14")
{
    std::vector<uint8_t> data(TcpHeaderSize + 4, 0);
    REQUIRE(WriteTcpSegment(data.data(), data.size(), Fields(TcpSyn)) == TcpHeaderSize);
    data[12] = 0x60;
    const uint8_t options[] = {3, 3, 15, 0};
    for (size_t index = 0; index < sizeof(options); ++index)
    {
        data[TcpHeaderSize + index] = options[index];
    }
    TcpSegment parsed{};
    REQUIRE(ParseTcpSegment(data.data(), data.size(), &parsed));
    REQUIRE(parsed.has_window_scale);
    REQUIRE(parsed.window_scale == TcpMaximumWindowScale);
    REQUIRE(parsed.maximum_segment_size == 0);
}

TEST_CASE("TCP options before the MSS are skipped")
{
    // NOP, NOP, a timestamp option, then the MSS.
    std::vector<uint8_t> data(TcpHeaderSize + 16, 0);
    REQUIRE(WriteTcpSegment(data.data(), data.size(), Fields(TcpSyn | TcpAck)) == TcpHeaderSize);
    data[12] = 0x90;
    const uint8_t options[] = {1, 1, 8, 10, 0, 0, 0, 1, 0, 0, 0, 2, 2, 4, 0x02, 0x18};
    for (size_t index = 0; index < sizeof(options); ++index)
    {
        data[TcpHeaderSize + index] = options[index];
    }
    TcpSegment parsed{};
    REQUIRE(ParseTcpSegment(data.data(), data.size(), &parsed));
    REQUIRE(parsed.maximum_segment_size == 536);
    REQUIRE(parsed.payload_length == 0);
}

TEST_CASE("a malformed TCP option ends the option list without an MSS")
{
    std::vector<uint8_t> data(TcpHeaderSize + 4, 0);
    REQUIRE(WriteTcpSegment(data.data(), data.size(), Fields(TcpSyn)) == TcpHeaderSize);
    data[12] = 0x60;
    data[TcpHeaderSize] = 2;
    data[TcpHeaderSize + 1] = 9;
    TcpSegment parsed{};
    REQUIRE(ParseTcpSegment(data.data(), data.size(), &parsed));
    REQUIRE(parsed.maximum_segment_size == 0);
}

TEST_CASE("a TCP header longer than the segment is refused")
{
    std::vector<uint8_t> data(TcpHeaderSize, 0);
    REQUIRE(WriteTcpSegment(data.data(), data.size(), Fields(TcpAck)) == TcpHeaderSize);
    TcpSegment parsed{};
    data[12] = 0x60;
    REQUIRE_FALSE(ParseTcpSegment(data.data(), data.size(), &parsed));
    data[12] = 0x40;
    REQUIRE_FALSE(ParseTcpSegment(data.data(), data.size(), &parsed));
    REQUIRE_FALSE(ParseTcpSegment(data.data(), TcpHeaderSize - 1, &parsed));
}

TEST_CASE("a TCP segment that does not fit is not written")
{
    std::vector<uint8_t> data(TcpHeaderSize + 1);
    const uint8_t payload[] = {1, 2};
    TcpSegment fields = Fields(TcpAck);
    fields.payload = payload;
    fields.payload_length = sizeof(payload);
    REQUIRE(WriteTcpSegment(data.data(), data.size(), fields) == 0);
    REQUIRE(WriteTcpSegment(data.data(), data.size(), Fields(TcpSyn, 1460)) == 0);
}

TEST_CASE("sequence numbers compare across the wrap")
{
    REQUIRE(SequenceBefore(1, 2));
    REQUIRE_FALSE(SequenceBefore(2, 2));
    REQUIRE(SequenceAtOrBefore(2, 2));
    REQUIRE(SequenceBefore(0xFFFFFFF0U, 0x10));
    REQUIRE_FALSE(SequenceBefore(0x10, 0xFFFFFFF0U));
}
