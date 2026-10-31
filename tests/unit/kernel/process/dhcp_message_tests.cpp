#include <catch2/catch_test_macros.hpp>

#include "user/dhcp_message.h"

#include <vector>

namespace Dhcp = FeatherOS::User::Dhcp;

namespace
{

constexpr uint8_t Mac[6] = {0x52, 0x54, 0x00, 0x12, 0x34, 0x56};
constexpr uint32_t Transaction = 0x12345678;

// A reply as QEMU's server sends it, with the options passed in after the message type.
std::vector<uint8_t> Reply(uint8_t type, std::vector<uint8_t> options)
{
    std::vector<uint8_t> message(240, 0);
    message[0] = 2;
    message[1] = 1;
    message[2] = 6;
    message[4] = 0x12;
    message[5] = 0x34;
    message[6] = 0x56;
    message[7] = 0x78;
    const uint8_t offered[4] = {10, 0, 2, 15};
    for (size_t index = 0; index < 4; ++index)
    {
        message[16 + index] = offered[index];
    }
    for (size_t index = 0; index < 6; ++index)
    {
        message[28 + index] = Mac[index];
    }
    message[236] = 99;
    message[237] = 130;
    message[238] = 83;
    message[239] = 99;
    message.insert(message.end(), {53, 1, type});
    message.insert(message.end(), options.begin(), options.end());
    message.push_back(255);
    return message;
}

} // namespace

TEST_CASE("a discover asks for a broadcast answer and the settings the client needs")
{
    uint8_t message[Dhcp::MessageSize];
    REQUIRE(Dhcp::BuildMessage(message, Dhcp::Discover, Transaction, Mac, nullptr, nullptr) ==
            Dhcp::MessageSize);
    REQUIRE(message[0] == 1);
    REQUIRE(message[1] == 1);
    REQUIRE(message[2] == 6);
    REQUIRE(message[4] == 0x12);
    REQUIRE(message[7] == 0x78);
    REQUIRE(message[10] == 0x80);
    REQUIRE(message[28] == 0x52);
    REQUIRE(message[33] == 0x56);
    REQUIRE(std::vector<uint8_t>(message + 236, message + 251) ==
            std::vector<uint8_t>{99, 130, 83, 99, 53, 1, 1, 55, 4, 1, 3, 6, 51, 255, 0});
}

TEST_CASE("a request names the offered address and the server")
{
    uint8_t message[Dhcp::MessageSize];
    const uint8_t requested[4] = {10, 0, 2, 15};
    const uint8_t server[4] = {10, 0, 2, 2};
    Dhcp::BuildMessage(message, Dhcp::Request, Transaction, Mac, requested, server);
    REQUIRE(std::vector<uint8_t>(message + 240, message + 255) ==
            std::vector<uint8_t>{53, 1, 3, 50, 4, 10, 0, 2, 15, 54, 4, 10, 0, 2, 2});
}

TEST_CASE("a renewal names only the client's address and takes a unicast answer")
{
    uint8_t message[Dhcp::MessageSize];
    const uint8_t address[4] = {10, 0, 2, 15};
    REQUIRE(Dhcp::BuildRenewal(message, Transaction, Mac, address) == Dhcp::MessageSize);
    REQUIRE(message[10] == 0);
    REQUIRE(std::vector<uint8_t>(message + 12, message + 16) == std::vector<uint8_t>{10, 0, 2, 15});
    REQUIRE(std::vector<uint8_t>(message + 240, message + 250) ==
            std::vector<uint8_t>{53, 1, 3, 55, 4, 1, 3, 6, 51, 255});
}

TEST_CASE("an acknowledgment gives the renewal and rebinding times")
{
    const std::vector<uint8_t> message =
        Reply(Dhcp::Ack, {51, 4, 0, 0, 0x0E, 0x10, 58, 4, 0, 0, 2, 0x58, 59, 4, 0, 0, 0x0B, 0xB8});
    Dhcp::Reply reply;
    REQUIRE(Dhcp::ParseReply(message.data(), message.size(), Transaction, Mac, &reply));
    REQUIRE(reply.lease_seconds == 3600);
    REQUIRE(reply.renewal_seconds == 600);
    REQUIRE(reply.rebinding_seconds == 3000);
}

TEST_CASE("an offer gives the address, netmask, router, name servers and lease")
{
    const std::vector<uint8_t> message =
        Reply(Dhcp::Offer, {0, 54, 4, 10, 0, 2, 2, 1, 4, 255, 255, 255, 0, 3, 8, 10, 0, 2, 2, 1,
                            1, 1, 1, 6, 16, 10, 0, 2, 3, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 51,
                            4, 0, 1, 0x51, 0x80});
    Dhcp::Reply reply;
    REQUIRE(Dhcp::ParseReply(message.data(), message.size(), Transaction, Mac, &reply));
    REQUIRE(reply.type == Dhcp::Offer);
    REQUIRE(reply.your_address[3] == 15);
    REQUIRE(reply.server[3] == 2);
    REQUIRE(reply.has_netmask);
    REQUIRE(Dhcp::NetmaskPrefixLength(reply.netmask) == 24);
    REQUIRE(reply.has_router);
    REQUIRE(reply.router[3] == 2);
    REQUIRE(reply.name_server_count == 3);
    REQUIRE(reply.name_servers[0][3] == 3);
    REQUIRE(reply.name_servers[2][0] == 2);
    REQUIRE(reply.lease_seconds == 86400);
}

TEST_CASE("replies for another client or with broken options are ignored")
{
    const std::vector<uint8_t> message = Reply(Dhcp::Ack, {});
    Dhcp::Reply reply;
    REQUIRE(Dhcp::ParseReply(message.data(), message.size(), Transaction, Mac, &reply));
    REQUIRE_FALSE(reply.has_router);
    REQUIRE(reply.lease_seconds == 0);
    REQUIRE(reply.renewal_seconds == 0);
    REQUIRE_FALSE(Dhcp::ParseReply(message.data(), message.size(), Transaction + 1, Mac, &reply));
    constexpr uint8_t Other[6] = {0x52, 0x54, 0x00, 0x12, 0x34, 0x57};
    REQUIRE_FALSE(Dhcp::ParseReply(message.data(), message.size(), Transaction, Other, &reply));

    std::vector<uint8_t> request = message;
    request[0] = 1;
    REQUIRE_FALSE(Dhcp::ParseReply(request.data(), request.size(), Transaction, Mac, &reply));
    std::vector<uint8_t> cookie = message;
    cookie[239] = 0;
    REQUIRE_FALSE(Dhcp::ParseReply(cookie.data(), cookie.size(), Transaction, Mac, &reply));
    const std::vector<uint8_t> overrun = Reply(Dhcp::Ack, {3, 8, 10, 0});
    REQUIRE_FALSE(Dhcp::ParseReply(overrun.data(), overrun.size() - 1, Transaction, Mac, &reply));
    std::vector<uint8_t> untyped = message;
    untyped[240] = 0;
    untyped[241] = 0;
    untyped[242] = 0;
    REQUIRE_FALSE(Dhcp::ParseReply(untyped.data(), untyped.size(), Transaction, Mac, &reply));
}

TEST_CASE("only netmasks without gaps have a prefix length")
{
    REQUIRE(Dhcp::NetmaskPrefixLength({255, 255, 255, 0}) == 24);
    REQUIRE(Dhcp::NetmaskPrefixLength({255, 255, 255, 255}) == 32);
    REQUIRE(Dhcp::NetmaskPrefixLength({0, 0, 0, 0}) == 0);
    REQUIRE(Dhcp::NetmaskPrefixLength({255, 0, 255, 0}) == -1);
}
