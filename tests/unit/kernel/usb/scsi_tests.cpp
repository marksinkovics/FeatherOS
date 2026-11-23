#include <catch2/catch_test_macros.hpp>

#include "kernel/usb/storage/scsi.h"

#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

namespace
{

using namespace FeatherOS::Usb::Storage::Scsi;

// CDB layouts: SPC-4 6.4 INQUIRY, 6.47 TEST UNIT READY, 6.39 REQUEST SENSE, 6.14 MODE SENSE(6),
// SBC-3 5.17 READ CAPACITY(10), 5.18 READ CAPACITY(16), 5.7/5.8 READ(10)/(16), 5.34/5.35
// WRITE(10)/(16), 5.20 SYNCHRONIZE CACHE(10), 5.21 START STOP UNIT. SCSI fields are big-endian.
std::vector<uint8_t> Bytes(const Cdb& cdb)
{
    return std::vector<uint8_t>(cdb.bytes, cdb.bytes + cdb.length);
}

std::span<const uint8_t> Span(const std::vector<uint8_t>& bytes)
{
    return std::span<const uint8_t>(bytes.data(), bytes.size());
}

bool Zero(const Cdb& cdb)
{
    for (uint32_t i = cdb.length; i < sizeof(cdb.bytes); i++)
    {
        if (cdb.bytes[i] != 0)
        {
            return false;
        }
    }
    return true;
}

TEST_CASE("scsi T8: fixed CDBs are byte-exact", "[usb][storage][scsi]")
{
    using V = std::vector<uint8_t>;
    CHECK(Bytes(Inquiry(36)) == V{0x12, 0x00, 0x00, 0x00, 0x24, 0x00});
    CHECK(Bytes(TestUnitReady()) == V{0x00, 0x00, 0x00, 0x00, 0x00, 0x00});
    CHECK(Bytes(RequestSense(18)) == V{0x03, 0x00, 0x00, 0x00, 0x12, 0x00});
    CHECK(Bytes(ReadCapacity10()) == V{0x25, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00});
    CHECK(Bytes(ReadCapacity16(32)) == V{0x9E, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
                                         0x00, 0x00, 0x00, 0x20, 0x00, 0x00});
    CHECK(Bytes(ModeSense6(0x3F, 192)) == V{0x1A, 0x00, 0x3F, 0x00, 0xC0, 0x00});
    CHECK(Bytes(SynchronizeCache()) ==
          V{0x35, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00});
    CHECK(Bytes(StartStopUnit(false, true)) == V{0x1B, 0x00, 0x00, 0x00, 0x02, 0x00});
    CHECK(Bytes(StartStopUnit(true, false)) == V{0x1B, 0x00, 0x00, 0x00, 0x01, 0x00});
    CHECK(Bytes(StartStopUnit(true, true)) == V{0x1B, 0x00, 0x00, 0x00, 0x03, 0x00});
    CHECK(Bytes(StartStopUnit(false, false)) == V{0x1B, 0x00, 0x00, 0x00, 0x00, 0x00});
    CHECK(Bytes(Inquiry(255))[4] == 0xFF);
    for (const Cdb& cdb : {Inquiry(36), TestUnitReady(), RequestSense(18), ReadCapacity10(),
                           ReadCapacity16(32), ModeSense6(0x3F, 192), SynchronizeCache(),
                           StartStopUnit(true, true), Read(1, 1), Write(1ULL << 33, 1)})
    {
        CHECK(Zero(cdb));
    }
}

TEST_CASE("scsi T8: READ and WRITE use big-endian fields", "[usb][storage][scsi]")
{
    using V = std::vector<uint8_t>;
    CHECK(Bytes(Read(0x01020304, 0x0506)) ==
          V{0x28, 0x00, 0x01, 0x02, 0x03, 0x04, 0x00, 0x05, 0x06, 0x00});
    CHECK(Bytes(Write(0x01020304, 0x0506)) ==
          V{0x2A, 0x00, 0x01, 0x02, 0x03, 0x04, 0x00, 0x05, 0x06, 0x00});
    CHECK(Bytes(Read(0x0102030405060708ULL, 0x090A0B0C)) ==
          V{0x88, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C,
            0x00, 0x00});
    CHECK(Bytes(Write(0x0102030405060708ULL, 0x090A0B0C)) ==
          V{0x8A, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C,
            0x00, 0x00});
    CHECK(Bytes(Read(0, 128)) == V{0x28, 0, 0, 0, 0, 0, 0, 0x00, 0x80, 0});
    CHECK(Bytes(Read(0, 16)) == V{0x28, 0, 0, 0, 0, 0, 0, 0x00, 0x10, 0});
}

TEST_CASE("scsi T8: the (10) and (16) forms switch at lba + blocks == 2^32", "[usb][storage][scsi]")
{
    using V = std::vector<uint8_t>;
    constexpr uint64_t Limit = 1ULL << 32;

    const Cdb last_ten = Read(Limit - 8, 8);
    CHECK(Bytes(last_ten) == V{0x28, 0x00, 0xFF, 0xFF, 0xFF, 0xF8, 0x00, 0x00, 0x08, 0x00});

    const Cdb first_sixteen = Read(Limit - 8, 9);
    CHECK(Bytes(first_sixteen) == V{0x88, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xF8,
                                    0x00, 0x00, 0x00, 0x09, 0x00, 0x00});

    CHECK(Read(Limit - 1, 1).bytes[0] == Opcode::Read10);
    CHECK(Read(Limit - 1, 2).bytes[0] == Opcode::Read16);
    CHECK(Read(Limit, 1).bytes[0] == Opcode::Read16);
    CHECK(Read(Limit + 5, 1).bytes[0] == Opcode::Read16);
    CHECK(Write(Limit - 128, 128).bytes[0] == Opcode::Write10);
    CHECK(Write(Limit - 128, 129).bytes[0] == Opcode::Write16);
    CHECK(Write(Limit - 128, 129).length == 16);
    CHECK(Write(Limit - 128, 128).length == 10);
    CHECK(Read(0, 0x10000).bytes[0] == Opcode::Read16);
    CHECK(Read(0, 0xFFFF).bytes[0] == Opcode::Read10);
}

TEST_CASE("scsi T8: classifying commands", "[usb][storage][scsi]")
{
    CHECK(IsReadOrWrite(Read(0, 1)));
    CHECK(IsReadOrWrite(Write(0, 1)));
    CHECK(IsReadOrWrite(Read(1ULL << 40, 1)));
    CHECK(IsReadOrWrite(Write(1ULL << 40, 1)));
    CHECK(!IsReadOrWrite(Inquiry(36)));
    CHECK(!IsReadOrWrite(SynchronizeCache()));
    CHECK(IsSynchronizeCache(SynchronizeCache()));
    CHECK(!IsSynchronizeCache(TestUnitReady()));
}

std::vector<uint8_t> InquiryResponse(uint8_t first, uint8_t second)
{
    std::vector<uint8_t> bytes(36, 0);
    bytes[0] = first;
    bytes[1] = second;
    bytes[4] = 31;
    std::memcpy(&bytes[8], "QEMU    ", 8);
    std::memcpy(&bytes[16], "QEMU HARDDISK   ", 16);
    std::memcpy(&bytes[32], "2.5+", 4);
    return bytes;
}

TEST_CASE("scsi T8: INQUIRY parsing", "[usb][storage][scsi]")
{
    InquiryData data{};
    auto bytes = InquiryResponse(0x00, 0x80);
    REQUIRE(ParseInquiry(Span(bytes), &data));
    CHECK(data.qualifier == 0);
    CHECK(data.type == 0);
    CHECK(data.removable);
    CHECK(std::strcmp(data.vendor, "QEMU") == 0);
    CHECK(std::strcmp(data.product, "QEMU HARDDISK") == 0);
    CHECK(std::strcmp(data.revision, "2.5+") == 0);
    CHECK(IsDisk(data));

    bytes = InquiryResponse(0x0E, 0x00);
    REQUIRE(ParseInquiry(Span(bytes), &data));
    CHECK(!data.removable);
    CHECK(IsDisk(data));

    bytes = InquiryResponse(0x05, 0x80);
    REQUIRE(ParseInquiry(Span(bytes), &data));
    CHECK(data.type == 5);
    CHECK(!IsDisk(data));

    bytes = InquiryResponse(0x20, 0x00);
    REQUIRE(ParseInquiry(Span(bytes), &data));
    CHECK(data.qualifier == 1);
    CHECK(!IsDisk(data));

    bytes = InquiryResponse(0x00, 0x00);
    std::memcpy(&bytes[8], "  AB\x01\x7F  ", 8);
    std::memset(&bytes[16], ' ', 16);
    std::memset(&bytes[32], 0, 4);
    REQUIRE(ParseInquiry(Span(bytes), &data));
    CHECK(std::strcmp(data.vendor, "AB??") == 0);
    CHECK(std::strcmp(data.product, "") == 0);
    CHECK(std::strcmp(data.revision, "") == 0);

    bytes = InquiryResponse(0x00, 0x00);
    std::memcpy(&bytes[16], "ABCDEFGHIJKLMNOP", 16);
    REQUIRE(ParseInquiry(Span(bytes), &data));
    CHECK(std::strcmp(data.product, "ABCDEFGHIJKLMNOP") == 0);
}

TEST_CASE("scsi T8: a truncated INQUIRY is refused", "[usb][storage][scsi]")
{
    InquiryData data{};
    const auto bytes = InquiryResponse(0x00, 0x00);
    for (size_t size : {size_t{0}, size_t{1}, size_t{8}, size_t{35}})
    {
        CHECK(!ParseInquiry(std::span<const uint8_t>(bytes.data(), size), &data));
    }
    CHECK(ParseInquiry(std::span<const uint8_t>(bytes.data(), 36), &data));
}

std::vector<uint8_t> Capacity10Response(uint32_t last_lba, uint32_t block_size)
{
    return {static_cast<uint8_t>(last_lba >> 24), static_cast<uint8_t>(last_lba >> 16),
            static_cast<uint8_t>(last_lba >> 8),  static_cast<uint8_t>(last_lba),
            static_cast<uint8_t>(block_size >> 24), static_cast<uint8_t>(block_size >> 16),
            static_cast<uint8_t>(block_size >> 8),  static_cast<uint8_t>(block_size)};
}

std::vector<uint8_t> Capacity16Response(uint64_t last_lba, uint32_t block_size)
{
    std::vector<uint8_t> bytes(32, 0);
    for (uint32_t i = 0; i < 8; i++)
    {
        bytes[i] = static_cast<uint8_t>(last_lba >> (56 - 8 * i));
    }
    for (uint32_t i = 0; i < 4; i++)
    {
        bytes[8 + i] = static_cast<uint8_t>(block_size >> (24 - 8 * i));
    }
    return bytes;
}

TEST_CASE("scsi T8: READ CAPACITY(10) parsing", "[usb][storage][scsi]")
{
    Capacity capacity{};
    auto bytes = Capacity10Response(131071, 512);
    REQUIRE(ParseCapacity10(Span(bytes), &capacity) == CapacityResult::Ok);
    CHECK(capacity.blocks == 131072);
    CHECK(capacity.block_size == 512);

    for (uint32_t size : {512U, 1024U, 2048U, 4096U})
    {
        bytes = Capacity10Response(99, size);
        REQUIRE(ParseCapacity10(Span(bytes), &capacity) == CapacityResult::Ok);
        CHECK(capacity.block_size == size);
    }

    bytes = Capacity10Response(0xFFFFFFFE, 512);
    REQUIRE(ParseCapacity10(Span(bytes), &capacity) == CapacityResult::Ok);
    CHECK(capacity.blocks == 0xFFFFFFFFULL);

    bytes = Capacity10Response(0xFFFFFFFF, 512);
    CHECK(ParseCapacity10(Span(bytes), &capacity) == CapacityResult::NeedSixteen);

    bytes = Capacity10Response(0, 512);
    REQUIRE(ParseCapacity10(Span(bytes), &capacity) == CapacityResult::Ok);
    CHECK(capacity.blocks == 1);

    for (uint32_t size : {0U, 256U, 520U, 8192U, 0x80000000U})
    {
        bytes = Capacity10Response(99, size);
        CHECK(ParseCapacity10(Span(bytes), &capacity) == CapacityResult::Invalid);
    }

    bytes = Capacity10Response(99, 512);
    CHECK(ParseCapacity10(std::span<const uint8_t>(bytes.data(), 7), &capacity) ==
          CapacityResult::Invalid);
    CHECK(ParseCapacity10(std::span<const uint8_t>(bytes.data(), 0), &capacity) ==
          CapacityResult::Invalid);
}

TEST_CASE("scsi T8: READ CAPACITY(16) parsing", "[usb][storage][scsi]")
{
    Capacity capacity{};
    auto bytes = Capacity16Response(0x1FFFFFFFFULL, 4096);
    REQUIRE(ParseCapacity16(Span(bytes), &capacity));
    CHECK(capacity.blocks == 0x200000000ULL);
    CHECK(capacity.block_size == 4096);

    bytes = Capacity16Response(0xFFFFFFFFULL, 512);
    REQUIRE(ParseCapacity16(Span(bytes), &capacity));
    CHECK(capacity.blocks == 0x100000000ULL);

    CHECK(ParseCapacity16(std::span<const uint8_t>(bytes.data(), 12), &capacity));
    CHECK(!ParseCapacity16(std::span<const uint8_t>(bytes.data(), 11), &capacity));
    CHECK(!ParseCapacity16(std::span<const uint8_t>(bytes.data(), 0), &capacity));

    bytes = Capacity16Response(99, 520);
    CHECK(!ParseCapacity16(Span(bytes), &capacity));
    bytes = Capacity16Response(UINT64_MAX, 512);
    CHECK(!ParseCapacity16(Span(bytes), &capacity));
    bytes = Capacity16Response(UINT64_MAX / 4096, 4096);
    CHECK(!ParseCapacity16(Span(bytes), &capacity));
    bytes = Capacity16Response(UINT64_MAX / 4096 - 1, 4096);
    CHECK(ParseCapacity16(Span(bytes), &capacity));
}

TEST_CASE("scsi T8: MODE SENSE(6) write protect", "[usb][storage][scsi]")
{
    bool protect = true;
    std::vector<uint8_t> bytes{0x03, 0x00, 0x00, 0x00};
    REQUIRE(ParseWriteProtect(Span(bytes), &protect));
    CHECK(!protect);
    bytes[2] = 0x80;
    REQUIRE(ParseWriteProtect(Span(bytes), &protect));
    CHECK(protect);
    bytes[2] = 0x10;
    REQUIRE(ParseWriteProtect(Span(bytes), &protect));
    CHECK(!protect);
    bytes[2] = 0xFF;
    REQUIRE(ParseWriteProtect(Span(bytes), &protect));
    CHECK(protect);
    CHECK(!ParseWriteProtect(std::span<const uint8_t>(bytes.data(), 3), &protect));
    CHECK(!ParseWriteProtect(std::span<const uint8_t>(bytes.data(), 0), &protect));
}

TEST_CASE("scsi T8: sense parsing, fixed and descriptor format", "[usb][storage][scsi]")
{
    Sense sense{};
    std::vector<uint8_t> fixed(18, 0);
    fixed[0] = 0x70;
    fixed[2] = 0x06;
    fixed[7] = 10;
    fixed[12] = 0x28;
    fixed[13] = 0x00;
    REQUIRE(ParseSense(Span(fixed), &sense));
    CHECK(sense.key == 6);
    CHECK(sense.asc == 0x28);
    CHECK(sense.ascq == 0);

    fixed[0] = 0xF1;
    fixed[2] = 0xF2;
    fixed[12] = 0x3A;
    fixed[13] = 0x02;
    REQUIRE(ParseSense(Span(fixed), &sense));
    CHECK(sense.key == 2);
    CHECK(sense.asc == 0x3A);
    CHECK(sense.ascq == 2);
    CHECK(ParseSense(std::span<const uint8_t>(fixed.data(), 14), &sense));
    CHECK(!ParseSense(std::span<const uint8_t>(fixed.data(), 13), &sense));

    std::vector<uint8_t> descriptor(8, 0);
    descriptor[0] = 0x72;
    descriptor[1] = 0x05;
    descriptor[2] = 0x24;
    descriptor[3] = 0x01;
    REQUIRE(ParseSense(Span(descriptor), &sense));
    CHECK(sense.key == 5);
    CHECK(sense.asc == 0x24);
    CHECK(sense.ascq == 1);
    descriptor[0] = 0x73;
    descriptor[1] = 0x0B;
    REQUIRE(ParseSense(Span(descriptor), &sense));
    CHECK(sense.key == 0xB);
    CHECK(!ParseSense(std::span<const uint8_t>(descriptor.data(), 7), &sense));
}

TEST_CASE("scsi T8: invalid sense is refused", "[usb][storage][scsi]")
{
    Sense sense{};
    CHECK(!ParseSense(std::span<const uint8_t>(), &sense));
    for (uint8_t code : {0x00, 0x6F, 0x74, 0x7F})
    {
        std::vector<uint8_t> bytes(18, 0);
        bytes[0] = code;
        CHECK(!ParseSense(Span(bytes), &sense));
    }
}

TEST_CASE("scsi T9: every row of the sense table", "[usb][storage][scsi]")
{
    const auto classify = [](uint8_t key, uint8_t asc, uint8_t ascq, bool sync = false)
    { return Classify(Sense{key, asc, ascq}, sync); };

    CHECK(classify(0, 0, 0) == Action::Success);
    CHECK(classify(1, 0x0B, 0x01) == Action::Success);
    CHECK(classify(1, 0, 0, true) == Action::Success);

    CHECK(classify(2, 0x04, 0x01) == Action::WaitAndRetry);
    CHECK(classify(2, 0x3A, 0x00) == Action::NoMedium);
    CHECK(classify(2, 0x3A, 0x02) == Action::NoMedium);
    CHECK(classify(2, 0x04, 0x02) == Action::Fail);
    CHECK(classify(2, 0x04, 0x00) == Action::Fail);
    CHECK(classify(2, 0x00, 0x00) == Action::Fail);

    CHECK(classify(3, 0x11, 0x00) == Action::Fail);
    CHECK(classify(3, 0x04, 0x01) == Action::Fail);
    CHECK(classify(4, 0x44, 0x00) == Action::Fail);

    CHECK(classify(5, 0x24, 0x00) == Action::Fail);
    CHECK(classify(5, 0x24, 0x00, true) == Action::Unsupported);
    CHECK(classify(5, 0x20, 0x00, false) == Action::Fail);

    CHECK(classify(6, 0x29, 0x00) == Action::Retry);
    CHECK(classify(6, 0x29, 0x01) == Action::Retry);
    CHECK(classify(6, 0x28, 0x00) == Action::MediumChanged);
    CHECK(classify(6, 0x3F, 0x00) == Action::Fail);
    CHECK(classify(6, 0x29, 0x00, true) == Action::Retry);

    CHECK(classify(7, 0x27, 0x00) == Action::WriteProtected);
    CHECK(classify(7, 0x00, 0x00) == Action::WriteProtected);

    CHECK(classify(0xB, 0x00, 0x00) == Action::Retry);
    CHECK(classify(0xB, 0x47, 0x00) == Action::Retry);

    for (uint8_t key : {8, 9, 0xA, 0xC, 0xD, 0xE, 0xF})
    {
        CHECK(classify(key, 0, 0) == Action::Fail);
    }
}

} // namespace
