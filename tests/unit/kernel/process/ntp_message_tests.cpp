#include <catch2/catch_test_macros.hpp>

#include "user/ntp_message.h"

#include <string>
#include <vector>

namespace Ntp = FeatherOS::User::Ntp;

namespace
{

// 2026-10-25 12:00:00 UTC, and the same in NTP seconds.
constexpr uint64_t UnixSeconds = 1792929600;
constexpr uint64_t NtpSeconds = UnixSeconds + 2208988800;
constexpr uint64_t Transmit = NtpSeconds << 32 | 0x80000000;

void Put64(std::vector<uint8_t>& packet, size_t offset, uint64_t value)
{
    for (size_t index = 0; index < 8; ++index)
    {
        packet[offset + index] = static_cast<uint8_t>(value >> (56 - index * 8));
    }
}

// A server's reply to Transmit, received one second and sent two seconds after it by its clock.
std::vector<uint8_t> Reply(uint8_t first = 0x24, uint8_t stratum = 2)
{
    std::vector<uint8_t> packet(48, 0);
    packet[0] = first;
    packet[1] = stratum;
    packet[12] = 'R';
    packet[13] = 'A';
    packet[14] = 'T';
    packet[15] = 'E';
    Put64(packet, 24, Transmit);
    Put64(packet, 32, Transmit + (1ULL << 32));
    Put64(packet, 40, Transmit + (2ULL << 32));
    return packet;
}

uint64_t ToUnix(uint64_t timestamp)
{
    uint64_t nanoseconds = 0;
    REQUIRE(Ntp::UnixNanosecondsFromTimestamp(timestamp, &nanoseconds));
    return nanoseconds;
}

} // namespace

TEST_CASE("NTP timestamps count from 1900 with a binary fraction")
{
    REQUIRE(Ntp::TimestampFromUnixNanoseconds(0) == 2208988800ULL << 32);
    REQUIRE(Ntp::TimestampFromUnixNanoseconds(UnixSeconds * 1000000000 + 500000000) == Transmit);
    REQUIRE(ToUnix(Transmit) == UnixSeconds * 1000000000 + 500000000);
    REQUIRE(ToUnix(2208988800ULL << 32) == 0);
    // A nanosecond survives the trip through 2^-32 s steps.
    for (const uint64_t nanoseconds : {1ULL, 123456789ULL, 999999999ULL})
    {
        const uint64_t time = UnixSeconds * 1000000000 + nanoseconds;
        REQUIRE(ToUnix(Ntp::TimestampFromUnixNanoseconds(time)) == time);
    }
    // The last fraction rounds up into the next second.
    REQUIRE(ToUnix(NtpSeconds << 32 | 0xFFFFFFFF) == (UnixSeconds + 1) * 1000000000);
}

TEST_CASE("NTP seconds wrap in 2036 and read as the next era below 1968")
{
    // 2036-02-07 06:28:16 UTC is the wrap; one second later reads as seconds 1.
    constexpr uint64_t WrapUnixSeconds = 2085978496;
    REQUIRE(Ntp::TimestampFromUnixNanoseconds((WrapUnixSeconds + 1) * 1000000000) == 1ULL << 32);
    REQUIRE(ToUnix(1ULL << 32) == (WrapUnixSeconds + 1) * 1000000000);
    REQUIRE(ToUnix(0x7FFFFFFFULL << 32) == (WrapUnixSeconds + 0x7FFFFFFF) * 1000000000);
    uint64_t nanoseconds = 0;
    // 1968 and 1969 are in era 0 but before the Unix epoch.
    REQUIRE_FALSE(Ntp::UnixNanosecondsFromTimestamp(0x80000000ULL << 32, &nanoseconds));
    REQUIRE_FALSE(Ntp::UnixNanosecondsFromTimestamp((2208988800ULL - 1) << 32, &nanoseconds));
}

TEST_CASE("an NTP request is a version 4 client packet carrying its transmit time")
{
    uint8_t packet[Ntp::PacketSize];
    for (uint8_t& byte : packet)
    {
        byte = 0xAA;
    }
    Ntp::BuildRequest(packet, Transmit);
    REQUIRE(packet[0] == 0x23);
    for (size_t index = 1; index < 40; ++index)
    {
        REQUIRE(packet[index] == 0);
    }
    REQUIRE(std::vector<uint8_t>(packet + 40, packet + 48) ==
            std::vector<uint8_t>{0xEE, 0x88, 0x6D, 0xC0, 0x80, 0x00, 0x00, 0x00});
}

TEST_CASE("a server reply gives its receive and transmit times")
{
    const std::vector<uint8_t> packet = Reply();
    Ntp::Reply reply = {};
    REQUIRE(Ntp::ParseReply(packet.data(), packet.size(), Transmit, &reply) ==
            Ntp::ReplyStatus::Valid);
    REQUIRE(reply.stratum == 2);
    REQUIRE(reply.receive == (UnixSeconds + 1) * 1000000000 + 500000000);
    REQUIRE(reply.transmit == (UnixSeconds + 2) * 1000000000 + 500000000);

    // Version 3 servers answer too, and extension fields after the header are ignored.
    std::vector<uint8_t> longer = Reply(0x1C);
    longer.resize(68, 0);
    REQUIRE(Ntp::ParseReply(longer.data(), longer.size(), Transmit, &reply) ==
            Ntp::ReplyStatus::Valid);
}

TEST_CASE("NTP replies that do not answer the request are malformed")
{
    Ntp::Reply reply = {};
    std::vector<uint8_t> packet = Reply();
    REQUIRE(Ntp::ParseReply(packet.data(), 47, Transmit, &reply) == Ntp::ReplyStatus::Malformed);
    REQUIRE(Ntp::ParseReply(nullptr, 48, Transmit, &reply) == Ntp::ReplyStatus::Malformed);
    REQUIRE(Ntp::ParseReply(packet.data(), packet.size(), Transmit + 1, &reply) ==
            Ntp::ReplyStatus::Malformed);
    // A client packet, a broadcast and version 5.
    for (const uint8_t first : {0x23, 0x25, 0x2C})
    {
        packet = Reply(first);
        REQUIRE(Ntp::ParseReply(packet.data(), packet.size(), Transmit, &reply) ==
                Ntp::ReplyStatus::Malformed);
    }
    packet = Reply();
    Put64(packet, 40, 0);
    REQUIRE(Ntp::ParseReply(packet.data(), packet.size(), Transmit, &reply) ==
            Ntp::ReplyStatus::Malformed);
    // A receive time before 1970.
    packet = Reply();
    Put64(packet, 32, 0x80000000ULL << 32);
    REQUIRE(Ntp::ParseReply(packet.data(), packet.size(), Transmit, &reply) ==
            Ntp::ReplyStatus::Malformed);
}

TEST_CASE("NTP servers can refuse with a kiss code or be unsynchronized")
{
    Ntp::Reply reply = {};
    std::vector<uint8_t> packet = Reply(0x24, 0);
    REQUIRE(Ntp::ParseReply(packet.data(), packet.size(), Transmit, &reply) ==
            Ntp::ReplyStatus::Kiss);
    REQUIRE(std::string(reply.kiss_code) == "RATE");
    packet[15] = 0x01;
    REQUIRE(Ntp::ParseReply(packet.data(), packet.size(), Transmit, &reply) ==
            Ntp::ReplyStatus::Kiss);
    REQUIRE(std::string(reply.kiss_code) == "RAT?");
    Put64(packet, 32, 0);
    Put64(packet, 40, 0);
    REQUIRE(Ntp::ParseReply(packet.data(), packet.size(), Transmit, &reply) ==
            Ntp::ReplyStatus::Kiss);
    // A kiss must still answer this request.
    REQUIRE(Ntp::ParseReply(packet.data(), packet.size(), Transmit - 1, &reply) ==
            Ntp::ReplyStatus::Malformed);

    packet = Reply(0xE4);
    REQUIRE(Ntp::ParseReply(packet.data(), packet.size(), Transmit, &reply) ==
            Ntp::ReplyStatus::Unsynchronized);
    packet = Reply(0x24, 16);
    REQUIRE(Ntp::ParseReply(packet.data(), packet.size(), Transmit, &reply) ==
            Ntp::ReplyStatus::Unsynchronized);
}

TEST_CASE("an NTP sample splits the round trip evenly between the two directions")
{
    constexpr uint64_t Second = 1000000000;
    constexpr uint64_t Sent = UnixSeconds * Second;
    // The server is 100 s ahead, the trip takes 30 ms each way and the server holds it for 5 ms.
    Ntp::Reply reply = {2, "", Sent + 100 * Second + 30000000, Sent + 100 * Second + 35000000};
    Ntp::Sample sample = Ntp::Measure(Sent, reply, Sent + 65000000);
    REQUIRE(sample.offset == 100 * static_cast<int64_t>(Second));
    REQUIRE(sample.round_trip_delay == 60000000);

    // A server years behind gives a negative offset; a half nanosecond rounds toward zero.
    reply = {2, "", Sent - 5 * 365 * 86400 * Second + 1, Sent - 5 * 365 * 86400 * Second + 2};
    sample = Ntp::Measure(Sent, reply, Sent + 2);
    REQUIRE(sample.offset == -static_cast<int64_t>(5 * 365 * 86400 * Second) + 1);
    REQUIRE(sample.round_trip_delay == 1);

    // Our clock did not tick during the trip, but the server held the request for 1 ms.
    reply = {2, "", Sent + 1000000, Sent + 2000000};
    REQUIRE(Ntp::Measure(Sent, reply, Sent).round_trip_delay == 0);
}
