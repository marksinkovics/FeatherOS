#include <catch2/catch_test_macros.hpp>

#include "kernel/storage/ahci/fis.h"
#include "kernel/storage/ahci/registers.h"

#include <array>
#include <bit>
#include <cstdint>
#include <cstring>
#include <vector>

namespace
{

using namespace FeatherOS::Storage::Ahci;

static_assert(std::endian::native == std::endian::little);

template <typename T> std::vector<uint8_t> Bytes(const T& value)
{
    std::vector<uint8_t> bytes(sizeof(value));
    std::memcpy(bytes.data(), &value, sizeof(value));
    return bytes;
}

} // namespace

TEST_CASE("ahci T15: READ DMA EXT H2D FIS bytes", "[ahci]")
{
    // Serial ATA "Register - Host to Device FIS": 27h, C in bit 7 of byte 1, command, features 7:0,
    // LBA 7:0, 15:8, 23:16, device (bit 6 = LBA), LBA 31:24, 39:32, 47:40, features 15:8, count
    // 7:0 and 15:8, ICC, control, four reserved bytes. READ DMA EXT is 25h (ATA ACS-4 7.22).
    const auto fis = BuildTransfer(false, 0xA1B2C3D4E5F6ULL, 0x0108);
    REQUIRE(fis.has_value());
    CHECK(Bytes(*fis) == std::vector<uint8_t>{0x27, 0x80, 0x25, 0x00, 0xF6, 0xE5, 0xD4, 0x40, 0xC3,
                                              0xB2, 0xA1, 0x00, 0x08, 0x01, 0x00, 0x00, 0x00, 0x00,
                                              0x00, 0x00});
}

TEST_CASE("ahci T15: WRITE DMA EXT H2D FIS bytes", "[ahci]")
{
    // WRITE DMA EXT is 35h (ATA ACS-4 7.58); the layout is the same as for a read.
    const auto fis = BuildTransfer(true, 1, 8);
    REQUIRE(fis.has_value());
    CHECK(Bytes(*fis) == std::vector<uint8_t>{0x27, 0x80, 0x35, 0x00, 0x01, 0x00, 0x00, 0x40, 0x00,
                                              0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00,
                                              0x00, 0x00});
}

TEST_CASE("ahci T15: 65536 sectors encode as a zero count", "[ahci]")
{
    // In the 48-bit commands a sector count of 0 means 65536 (ATA ACS-4 7.22.3).
    const auto fis = BuildTransfer(false, 0, 65536);
    REQUIRE(fis.has_value());
    CHECK(fis->count[0] == 0);
    CHECK(fis->count[1] == 0);
}

TEST_CASE("ahci T15: transfers outside the command limits are refused", "[ahci]")
{
    CHECK_FALSE(BuildTransfer(false, 0, 0).has_value());
    CHECK_FALSE(BuildTransfer(false, 0, 65537).has_value());
    CHECK_FALSE(BuildTransfer(true, LbaLimit, 1).has_value());
    CHECK_FALSE(BuildTransfer(true, LbaLimit - 1, 2).has_value());
    CHECK(BuildTransfer(true, LbaLimit - 1, 1).has_value());
}

TEST_CASE("ahci T15: commands without data", "[ahci]")
{
    // FLUSH CACHE EXT EAh, FLUSH CACHE E7h, STANDBY IMMEDIATE E0h, IDENTIFY DEVICE ECh: no LBA and
    // no count, device register still 40h as the driver sends it for every command.
    const auto none = [](uint8_t command)
    {
        return std::vector<uint8_t>{0x27, 0x80, command, 0, 0, 0, 0, 0x40, 0, 0, 0, 0, 0, 0, 0, 0,
                                    0,    0,    0,       0};
    };
    CHECK(Bytes(BuildNoData(AtaCommand::FlushCacheExt)) == none(0xEA));
    CHECK(Bytes(BuildNoData(AtaCommand::FlushCache)) == none(0xE7));
    CHECK(Bytes(BuildNoData(AtaCommand::StandbyImmediate)) == none(0xE0));
    CHECK(Bytes(BuildNoData(AtaCommand::IdentifyDevice)) == none(0xEC));
}

TEST_CASE("ahci T15: command header packing", "[ahci]")
{
    // AHCI 1.3.1 4.2.2: DW0 CFL (4:0) = FIS length in dwords = 5, W = bit 6, PRDTL = bits 31:16;
    // DW1 PRDBC; DW2 CTBA 31:7 (128-byte aligned) and DW3 CTBAU.
    const auto read = BuildCommandHeader(false, 1, 0x1'2345'6780ULL);
    REQUIRE(read.has_value());
    CHECK(read->flags == 0x0001'0005);
    CHECK(read->prdbc == 0);
    CHECK(read->table_low == 0x2345'6780);
    CHECK(read->table_high == 0x1);

    const auto write = BuildCommandHeader(true, 8, 0x500);
    REQUIRE(write.has_value());
    CHECK(write->flags == 0x0008'0045);

    CHECK(Bytes(*write) == std::vector<uint8_t>{0x45, 0x00, 0x08, 0x00, 0, 0, 0, 0, 0x00, 0x05, 0,
                                                0,    0,    0,    0,    0, 0, 0, 0, 0, 0,    0, 0,
                                                0,    0,    0,    0,    0, 0, 0, 0, 0});
}

TEST_CASE("ahci T15: a command table off the 128-byte alignment is refused", "[ahci]")
{
    CHECK_FALSE(BuildCommandHeader(false, 1, 0x540).has_value());
    CHECK_FALSE(BuildCommandHeader(false, 1, 0x501).has_value());
    CHECK(BuildCommandHeader(false, 1, 0x580).has_value());
}

TEST_CASE("ahci T15: PRD DBC is bytes minus one with bit 0 set", "[ahci]")
{
    // AHCI 1.3.1 4.2.3: DBC bits 21:0 hold the byte count minus one, and bit 0 must be 1 (an even
    // number of bytes); bit 31 is the interrupt-on-completion flag.
    const auto prd = BuildPrd(0x1'2345'6000ULL, 512, false);
    REQUIRE(prd.has_value());
    CHECK(prd->address_low == 0x2345'6000);
    CHECK(prd->address_high == 0x1);
    CHECK(prd->reserved == 0);
    CHECK(prd->count == 511);
    CHECK((prd->count & 1) == 1);
    CHECK(PrdByteCount(*prd) == 512);

    const auto interrupting = BuildPrd(0x1000, 128 * 1024, true);
    REQUIRE(interrupting.has_value());
    CHECK(interrupting->count == (0x80000000U | 0x1FFFF));
    CHECK(PrdByteCount(*interrupting) == 128 * 1024);

    CHECK(Bytes(*prd) == std::vector<uint8_t>{0x00, 0x60, 0x45, 0x23, 0x01, 0, 0, 0, 0, 0, 0, 0,
                                              0xFF, 0x01, 0x00, 0x00});
}

TEST_CASE("ahci T15: PRD refuses odd byte counts and odd addresses", "[ahci]")
{
    CHECK_FALSE(BuildPrd(0x1000, 511, false).has_value());
    CHECK_FALSE(BuildPrd(0x1000, 1, false).has_value());
    CHECK_FALSE(BuildPrd(0x1001, 512, false).has_value());
    CHECK_FALSE(BuildPrd(0x1'0000'0001ULL, 512, false).has_value());
    CHECK(BuildPrd(0x1002, 2, false).has_value());
}

TEST_CASE("ahci T15: PRD refuses zero and more than 4 MiB", "[ahci]")
{
    CHECK_FALSE(BuildPrd(0x1000, 0, false).has_value());
    CHECK_FALSE(BuildPrd(0x1000, MaxPrdBytes + 2, false).has_value());
    const auto biggest = BuildPrd(0x1000, MaxPrdBytes, false);
    REQUIRE(biggest.has_value());
    CHECK(biggest->count == 0x3FFFFF);
    CHECK(PrdByteCount(*biggest) == MaxPrdBytes);
}

TEST_CASE("ahci T15: device signatures", "[ahci]")
{
    CHECK(ClassifySignature(0x00000101) == DeviceKind::Ata);
    CHECK(ClassifySignature(0xEB140101) == DeviceKind::Atapi);
    CHECK(ClassifySignature(0x96690101) == DeviceKind::PortMultiplier);
    CHECK(ClassifySignature(0xC33C0101) == DeviceKind::Semb);
    CHECK(ClassifySignature(0xFFFFFFFF) == DeviceKind::Unknown);
    CHECK(ClassifySignature(0) == DeviceKind::Unknown);
    CHECK(ClassifySignature(0x00000102) == DeviceKind::Unknown);
}

TEST_CASE("ahci T15: CAP decoding", "[ahci]")
{
    // AHCI 1.3.1 3.1.1: NP 4:0, NCS 12:8 (both zero-based), SAM 18, SSS 27, SNCQ 30, S64A 31.
    const auto cap = DecodeCap(0xC0101F45);
    CHECK(cap.port_count == 6);
    CHECK(cap.command_slots == 32);
    CHECK_FALSE(cap.ahci_only);
    CHECK(cap.staggered_spin_up == false);
    CHECK(cap.ncq);
    CHECK(cap.addresses_64bit);

    const auto flags = DecodeCap((1U << 18) | (1U << 27) | 0x0200);
    CHECK(flags.port_count == 1);
    CHECK(flags.command_slots == 3);
    CHECK(flags.ahci_only);
    CHECK(flags.staggered_spin_up);
    CHECK_FALSE(flags.ncq);
    CHECK_FALSE(flags.addresses_64bit);
}

TEST_CASE("ahci T15: register offsets and ports", "[ahci]")
{
    CHECK(PortRegister(0, PortReg::Cmd) == 0x118);
    CHECK(PortRegister(2, PortReg::Ci) == 0x238);
    CHECK(PortRegister(31, PortReg::Clb) == 0x1080);
    CHECK(PortImplemented(0b101, 0));
    CHECK_FALSE(PortImplemented(0b101, 1));
    CHECK(PortImplemented(0b101, 2));
    CHECK_FALSE(PortImplemented(0xFFFFFFFF, 32));
    CHECK(HasHandoff(1));
    CHECK_FALSE(HasHandoff(0xFFFFFFFE));
    CHECK(DecodeVersion(0x00010301).major == 1);
    CHECK(DecodeVersion(0x00010301).minor == 0x0301);
}
