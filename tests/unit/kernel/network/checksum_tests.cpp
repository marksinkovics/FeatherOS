#include <catch2/catch_test_macros.hpp>

#include "kernel/network/checksum.h"

using namespace FeatherOS::Network;

TEST_CASE("the Internet checksum matches RFC 1071's example")
{
    const uint8_t data[] = {0x00, 0x01, 0xF2, 0x03, 0xF4, 0xF5, 0xF6, 0xF7};
    // The words sum to 0x2DDF0, which folds to 0xDDF2.
    REQUIRE(InternetChecksum(data, sizeof(data)) == static_cast<uint16_t>(~0xDDF2));
}

TEST_CASE("an odd final byte is padded with zero")
{
    const uint8_t data[] = {0x01};
    REQUIRE(InternetChecksum(data, sizeof(data)) == 0xFEFF);
}

TEST_CASE("checksums of even-length parts add up to the checksum of the whole")
{
    const uint8_t data[] = {0x45, 0x00, 0x00, 0x73, 0x00, 0x00, 0x40, 0x00, 0x40, 0x11, 0x01};
    const uint64_t parts = ChecksumAdd(ChecksumAdd(0, data, 4), data + 4, sizeof(data) - 4);
    REQUIRE(ChecksumFinish(parts) == InternetChecksum(data, sizeof(data)));
}

TEST_CASE("data that holds its correct checksum sums to zero")
{
    uint8_t data[] = {0x12, 0x34, 0x00, 0x00, 0xAB, 0xCD};
    const uint16_t checksum = InternetChecksum(data, sizeof(data));
    data[2] = static_cast<uint8_t>(checksum >> 8);
    data[3] = static_cast<uint8_t>(checksum);
    REQUIRE(InternetChecksum(data, sizeof(data)) == 0);
}
