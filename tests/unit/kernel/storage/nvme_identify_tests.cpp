#include <catch2/catch_test_macros.hpp>

#include "kernel/storage/nvme/identify.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>

namespace
{

using namespace FeatherOS::Storage::Nvme;

using Page = std::array<uint8_t, IdentifyBytes>;

void Put(Page& page, size_t offset, uint64_t value, size_t bytes)
{
    for (size_t i = 0; i < bytes; i++)
    {
        page[offset + i] = static_cast<uint8_t>(value >> (8 * i));
    }
}

void PutText(Page& page, size_t offset, size_t size, std::string_view text)
{
    std::memset(&page[offset], ' ', size);
    std::memcpy(&page[offset], text.data(), text.size());
}

// Identify Controller data structure: VID 1:0, SSVID 3:2, SN 23:4, MN 63:24, FR 71:64, MDTS 77,
// VER 83:80, RTD3E 91:88, SQES 512, CQES 513, NN 519:516, VWC 525.
Page Controller()
{
    Page page{};
    Put(page, 0, 0x1B36, 2);
    Put(page, 2, 0x1AF4, 2);
    PutText(page, 4, 20, "deadbeef");
    PutText(page, 24, 40, "QEMU NVMe Ctrl");
    PutText(page, 64, 8, "1.0");
    page[77] = 5;
    Put(page, 80, 0x00010400, 4);
    Put(page, 88, 8000000, 4);
    page[512] = 0x66;
    page[513] = 0x44;
    Put(page, 516, 3, 4);
    page[525] = 1;
    return page;
}

// Identify Namespace data structure: NSZE 7:0, NCAP 15:8, NUSE 23:16, NLBAF 25, FLBAS 26, DPS 29,
// NSATTR 99, LBAF0 at 128 (4 bytes each: MS 15:0, LBADS 23:16, RP 25:24).
constexpr uint32_t Format(uint16_t metadata, uint8_t lbads)
{
    return metadata | (static_cast<uint32_t>(lbads) << 16);
}

Page Namespace(uint64_t size = 262144)
{
    Page page{};
    Put(page, 0, size, 8);
    Put(page, 8, size, 8);
    Put(page, 16, size / 2, 8);
    page[25] = 1;
    page[26] = 0;
    Put(page, 128, Format(0, 9), 4);
    Put(page, 132, Format(0, 12), 4);
    return page;
}

ControllerData Parse(const Page& page)
{
    return ParseController(std::span<const uint8_t, IdentifyBytes>(page));
}

NamespaceData ParseNs(const Page& page)
{
    return ParseNamespace(std::span<const uint8_t, IdentifyBytes>(page));
}

} // namespace

TEST_CASE("nvme T6: Identify Controller fields", "[nvme]")
{
    const ControllerData data = Parse(Controller());
    CHECK(data.vendor_id == 0x1B36);
    CHECK(data.subsystem_vendor_id == 0x1AF4);
    CHECK(std::string_view(data.serial) == "deadbeef");
    CHECK(std::string_view(data.model) == "QEMU NVMe Ctrl");
    CHECK(std::string_view(data.firmware) == "1.0");
    CHECK(data.version == 0x00010400);
    CHECK(data.namespace_count == 3);
    CHECK(data.rtd3_entry_latency_us == 8000000);
    CHECK(data.volatile_write_cache);
    CHECK(data.max_transfer_bytes == 128 * 1024);
    CHECK(SupportsEntrySizes(data));
}

TEST_CASE("nvme T6: MDTS 0 is unlimited and N is 4 KiB << N", "[nvme]")
{
    Page page = Controller();
    page[77] = 0;
    ControllerData data = Parse(page);
    CHECK(data.max_transfer_bytes == 0);
    CHECK(BounceBufferBytes(data) == 128 * 1024);

    const struct
    {
        uint8_t mdts;
        uint64_t bytes;
        uint32_t bounce;
    } table[] = {
        {1, 8192, 8192},
        {2, 16384, 16384},
        {5, 128 * 1024, 128 * 1024},
        {6, 256 * 1024, 128 * 1024},
        {10, 4 * 1024 * 1024, 128 * 1024},
        {255, 4096ULL << 20, 128 * 1024},
    };
    for (const auto& row : table)
    {
        CAPTURE(row.mdts);
        page[77] = row.mdts;
        data = Parse(page);
        CHECK(data.max_transfer_bytes == row.bytes);
        CHECK(BounceBufferBytes(data) == row.bounce);
    }
}

TEST_CASE("nvme T6: VWC, NN and RTD3E come from their own bytes", "[nvme]")
{
    Page page = Controller();
    page[525] = 0xFE;
    CHECK_FALSE(Parse(page).volatile_write_cache);
    page[525] = 0x01;
    CHECK(Parse(page).volatile_write_cache);

    Put(page, 516, 0xFFFFFFFE, 4);
    CHECK(Parse(page).namespace_count == 0xFFFFFFFE);
    Put(page, 516, 0, 4);
    CHECK(Parse(page).namespace_count == 0);

    Put(page, 88, 0, 4);
    CHECK(Parse(page).rtd3_entry_latency_us == 0);
    Put(page, 88, 0xFFFFFFFF, 4);
    CHECK(Parse(page).rtd3_entry_latency_us == 0xFFFFFFFF);
}

TEST_CASE("nvme T6: the queue entry sizes must allow 64 and 16 bytes", "[nvme]")
{
    Page page = Controller();
    CHECK(SupportsEntrySizes(Parse(page)));
    page[512] = 0x76;
    CHECK(SupportsEntrySizes(Parse(page)));
    page[512] = 0x77;
    CHECK_FALSE(SupportsEntrySizes(Parse(page)));
    page[512] = 0x55;
    CHECK_FALSE(SupportsEntrySizes(Parse(page)));
    page[512] = 0x66;
    page[513] = 0x55;
    CHECK_FALSE(SupportsEntrySizes(Parse(page)));
    page[513] = 0x54;
    CHECK(SupportsEntrySizes(Parse(page)));
}

TEST_CASE("nvme T6: strings are trimmed, terminated and printable", "[nvme]")
{
    Page page = Controller();
    std::memset(&page[4], 'A', 20);
    std::memset(&page[24], 'B', 40);
    std::memset(&page[64], 'C', 8);
    page[10] = 0x01;
    ControllerData data = Parse(page);
    CHECK(std::string_view(data.serial) == "AAAAAA?AAAAAAAAAAAAA");
    CHECK(std::string_view(data.model).size() == 40);
    CHECK(std::string_view(data.firmware).size() == 8);

    std::memset(&page[4], ' ', 20);
    data = Parse(page);
    CHECK(std::string_view(data.serial).empty());
}

TEST_CASE("nvme T6: Identify Namespace accepts an ordinary namespace", "[nvme]")
{
    NamespaceData data = ParseNs(Namespace());
    CHECK(data.refusal == Refusal::None);
    CHECK(data.sector_count == 262144);
    CHECK(data.used_sectors == 131072);
    CHECK(data.sector_size == 512);
    CHECK(data.metadata_bytes == 0);
    CHECK(data.format_index == 0);
    CHECK_FALSE(data.write_protected);

    Page page = Namespace();
    page[26] = 1;
    data = ParseNs(page);
    CHECK(data.refusal == Refusal::None);
    CHECK(data.sector_size == 4096);
    CHECK(data.format_index == 1);
}

TEST_CASE("nvme T6: FLBAS bits 3:0 select the format, 6:5 add the upper bits above 16 formats",
          "[nvme]")
{
    Page page = Namespace();
    page[25] = 40;
    for (uint32_t i = 0; i <= 40; i++)
    {
        Put(page, 128 + 4 * i, Format(8, 12), 4);
    }
    Put(page, 128 + 4 * 3, Format(0, 9), 4);
    Put(page, 128 + 4 * 21, Format(0, 12), 4);
    Put(page, 128 + 4 * 35, Format(0, 9), 4);

    page[26] = 3;
    NamespaceData data = ParseNs(page);
    CHECK(data.format_index == 3);
    CHECK(data.refusal == Refusal::None);
    CHECK(data.sector_size == 512);

    page[26] = 5 | (1 << 5);
    data = ParseNs(page);
    CHECK(data.format_index == 21);
    CHECK(data.refusal == Refusal::None);
    CHECK(data.sector_size == 4096);

    page[26] = 3 | (2 << 5);
    data = ParseNs(page);
    CHECK(data.format_index == 35);
    CHECK(data.refusal == Refusal::None);
    CHECK(data.sector_size == 512);

    page[26] = 0x10 | 3;
    data = ParseNs(page);
    CHECK(data.format_index == 3);

    page[26] = 15 | (3 << 5);
    data = ParseNs(page);
    CHECK(data.refusal == Refusal::BadFormat);
}

TEST_CASE("nvme T6: the upper FLBAS bits are ignored with 16 formats or fewer", "[nvme]")
{
    Page page = Namespace();
    page[25] = 15;
    Put(page, 128 + 4 * 16, Format(8, 9), 4);
    page[26] = 0x20;
    const NamespaceData data = ParseNs(page);
    CHECK(data.format_index == 0);
    CHECK(data.refusal == Refusal::None);
}

TEST_CASE("nvme T6: a format index past NLBAF is refused", "[nvme]")
{
    Page page = Namespace();
    page[26] = 2;
    CHECK(ParseNs(page).refusal == Refusal::BadFormat);
}

TEST_CASE("nvme T6: namespaces with metadata are refused (I11)", "[nvme]")
{
    Page page = Namespace();
    Put(page, 128, Format(8, 9), 4);
    NamespaceData data = ParseNs(page);
    CHECK(data.refusal == Refusal::Metadata);
    CHECK(data.metadata_bytes == 8);

    Put(page, 128, Format(64, 12), 4);
    CHECK(ParseNs(page).refusal == Refusal::Metadata);

    Put(page, 128, Format(1, 9), 4);
    CHECK(ParseNs(page).refusal == Refusal::Metadata);
}

TEST_CASE("nvme T6: only sector sizes 512 and 4096 are accepted (I11)", "[nvme]")
{
    Page page = Namespace();
    for (const uint8_t lbads : {0, 8, 10, 11, 13, 16})
    {
        CAPTURE(lbads);
        Put(page, 128, Format(0, lbads), 4);
        CHECK(ParseNs(page).refusal == Refusal::SectorSize);
    }
    for (const uint8_t lbads : {9, 12})
    {
        CAPTURE(lbads);
        Put(page, 128, Format(0, lbads), 4);
        CHECK(ParseNs(page).refusal == Refusal::None);
    }
}

TEST_CASE("nvme T6: protection information is refused (I11)", "[nvme]")
{
    Page page = Namespace();
    for (const uint8_t dps : {1, 2, 3, 7, 0x09, 0x0F})
    {
        CAPTURE(dps);
        page[29] = dps;
        CHECK(ParseNs(page).refusal == Refusal::ProtectionInfo);
    }
    page[29] = 0x08;
    CHECK(ParseNs(page).refusal == Refusal::None);
    page[29] = 0;
    CHECK(ParseNs(page).refusal == Refusal::None);
}

TEST_CASE("nvme T6: an empty namespace size means the namespace is not active (I11)", "[nvme]")
{
    Page page = Namespace();
    Put(page, 0, 0, 8);
    CHECK(ParseNs(page).refusal == Refusal::Inactive);
    CHECK(ParseNs(Page{}).refusal == Refusal::Inactive);

    Put(page, 0, 1, 8);
    CHECK(ParseNs(page).refusal == Refusal::None);
    Put(page, 0, 0x8000000000000000ULL, 8);
    CHECK(ParseNs(page).sector_count == 0x8000000000000000ULL);
}

TEST_CASE("nvme T6: NSATTR bit 0 marks a write-protected namespace", "[nvme]")
{
    Page page = Namespace();
    page[99] = 0xFE;
    CHECK_FALSE(ParseNs(page).write_protected);
    page[99] = 0x01;
    const NamespaceData data = ParseNs(page);
    CHECK(data.write_protected);
    CHECK(data.refusal == Refusal::None);
}

TEST_CASE("nvme T6: every refusal has a text", "[nvme]")
{
    for (const Refusal r : {Refusal::None, Refusal::Inactive, Refusal::BadFormat, Refusal::Metadata,
                            Refusal::SectorSize, Refusal::ProtectionInfo})
    {
        CHECK(std::string_view(RefusalText(r)).size() > 0);
    }
}

TEST_CASE("nvme T6: the active namespace list stops at the first 0", "[nvme]")
{
    Page page{};
    Put(page, 0, 1, 4);
    Put(page, 4, 2, 4);
    Put(page, 8, 5, 4);
    Put(page, 16, 9, 4);

    std::array<uint32_t, 8> out{};
    CHECK(ParseActiveList(page, out) == 3);
    CHECK(out[0] == 1);
    CHECK(out[1] == 2);
    CHECK(out[2] == 5);
    CHECK(out[3] == 0);

    CHECK(ParseActiveList(Page{}, out) == 0);
}

TEST_CASE("nvme T6: the active namespace list is capped by the output and by garbage", "[nvme]")
{
    Page page{};
    for (uint32_t i = 0; i < 20; i++)
    {
        Put(page, 4 * i, i + 1, 4);
    }
    std::array<uint32_t, MaxNamespaces> out{};
    CHECK(ParseActiveList(page, out) == MaxNamespaces);
    CHECK(out[MaxNamespaces - 1] == MaxNamespaces);

    std::array<uint32_t, 2> small{};
    CHECK(ParseActiveList(page, small) == 2);

    Page unordered{};
    Put(unordered, 0, 4, 4);
    Put(unordered, 4, 3, 4);
    Put(unordered, 8, 7, 4);
    CHECK(ParseActiveList(unordered, out) == 1);

    Page broadcast{};
    Put(broadcast, 0, 1, 4);
    Put(broadcast, 4, 0xFFFFFFFF, 4);
    CHECK(ParseActiveList(broadcast, out) == 1);

    Page full{};
    for (uint32_t i = 0; i < 1024; i++)
    {
        Put(full, 4 * i, i + 1, 4);
    }
    std::array<uint32_t, 2048> big{};
    CHECK(ParseActiveList(full, big) == 1024);
    CHECK(big[1023] == 1024);
}
