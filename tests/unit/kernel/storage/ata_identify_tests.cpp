#include <catch2/catch_test_macros.hpp>

#include "kernel/storage/ata/identify.h"

#include <array>
#include <cstdint>
#include <cstring>
#include <string_view>

namespace
{

using namespace FeatherOS::Storage::Ata;

using Page = std::array<uint16_t, IdentifyWords>;

// ATA ACS-4 IDENTIFY DEVICE: strings hold the first character in the high byte of each word,
// word 82 and 83 bit 14 are validity patterns (15:14 = 01), word 83 bit 10 is 48-bit LBA, bit 12
// FLUSH CACHE, bit 13 FLUSH CACHE EXT; word 85 bit 5 and word 82 bit 5 are the write cache; words
// 100-103 the 48-bit sector count; word 106 and words 117-118 the logical sector size; word 255
// has A5h in its low byte and the checksum in its high byte.
void PutText(Page& page, size_t first, size_t chars, std::string_view text)
{
    for (size_t i = 0; i < chars; i++)
    {
        const char c = i < text.size() ? text[i] : ' ';
        uint16_t& word = page[first + i / 2];
        if (i % 2 == 0)
        {
            word = static_cast<uint16_t>((word & 0x00FF) | (static_cast<uint8_t>(c) << 8));
        }
        else
        {
            word = static_cast<uint16_t>((word & 0xFF00) | static_cast<uint8_t>(c));
        }
    }
}

void Seal(Page& page)
{
    page[255] = 0x00A5;
    uint8_t sum = 0;
    for (const uint16_t word : page)
    {
        sum = static_cast<uint8_t>(sum + (word & 0xFF) + (word >> 8));
    }
    page[255] = static_cast<uint16_t>(0x00A5 | (static_cast<uint8_t>(0 - sum) << 8));
}

Page GoodDisk()
{
    Page page{};
    PutText(page, 10, 20, "feather-sata0");
    PutText(page, 23, 8, "2.5+");
    PutText(page, 27, 40, "QEMU HARDDISK");
    page[82] = 0x4000 | (1U << 5);
    page[83] = 0x4000 | (1U << 13) | (1U << 12) | (1U << 10);
    page[85] = 0x4000 | (1U << 5);
    page[86] = (1U << 13) | (1U << 12) | (1U << 10);
    page[100] = 0x0000;
    page[101] = 0x0005;
    return page;
}

AtaIdentity Parse(const Page& page)
{
    return ParseIdentify(std::span<const uint16_t, IdentifyWords>(page));
}

} // namespace

TEST_CASE("ata T16: an LBA48 disk is accepted", "[ata]")
{
    const AtaIdentity id = Parse(GoodDisk());
    CHECK(id.refusal == Refusal::None);
    CHECK(id.lba48);
    CHECK(id.sector_count == 0x50000);
    CHECK(id.sector_size == 512);
    CHECK_FALSE(id.checksum_present);
}

TEST_CASE("ata T16: sector count comes from words 100 to 103", "[ata]")
{
    Page page = GoodDisk();
    page[100] = 0x3210;
    page[101] = 0x7654;
    page[102] = 0xBA98;
    page[103] = 0x0000;
    CHECK(Parse(page).sector_count == 0x0000BA9876543210ULL);
}

TEST_CASE("ata T16: a disk without LBA48 is refused", "[ata]")
{
    Page absent = GoodDisk();
    absent[83] &= static_cast<uint16_t>(~(1U << 10));
    CHECK(Parse(absent).refusal == Refusal::NoLba48);
    CHECK_FALSE(Parse(absent).lba48);

    Page disabled = GoodDisk();
    disabled[86] &= static_cast<uint16_t>(~(1U << 10));
    CHECK(Parse(disabled).refusal == Refusal::NoLba48);

    Page invalid = GoodDisk();
    invalid[83] = (1U << 10);
    CHECK(Parse(invalid).refusal == Refusal::NoLba48);
}

TEST_CASE("ata T16: logical sector sizes", "[ata]")
{
    Page page = GoodDisk();
    page[106] = 0x4000 | (1U << 12);
    page[117] = 2048;
    page[118] = 0;
    AtaIdentity id = Parse(page);
    CHECK(id.refusal == Refusal::None);
    CHECK(id.sector_size == 4096);

    page[117] = 256;
    id = Parse(page);
    CHECK(id.refusal == Refusal::None);
    CHECK(id.sector_size == 512);

    page[117] = 1024;
    id = Parse(page);
    CHECK(id.refusal == Refusal::SectorSize);
    CHECK(id.sector_size == 2048);

    page[117] = 0;
    id = Parse(page);
    CHECK(id.refusal == Refusal::SectorSize);

    page[117] = 2048;
    page[106] = (1U << 12);
    id = Parse(page);
    CHECK(id.sector_size == 512);
    CHECK(id.refusal == Refusal::None);

    page[106] = 0x4000 | (1U << 13);
    CHECK(Parse(page).sector_size == 512);
}

TEST_CASE("ata T16: an empty disk is refused", "[ata]")
{
    Page page = GoodDisk();
    page[101] = 0;
    CHECK(Parse(page).refusal == Refusal::Empty);
}

TEST_CASE("ata T16: write cache and flush support", "[ata]")
{
    AtaIdentity id = Parse(GoodDisk());
    CHECK(id.write_cache_supported);
    CHECK(id.write_cache_enabled);
    CHECK(id.flush_cache_supported);
    CHECK(id.flush_cache_ext_supported);

    Page off = GoodDisk();
    off[85] &= static_cast<uint16_t>(~(1U << 5));
    id = Parse(off);
    CHECK(id.write_cache_supported);
    CHECK_FALSE(id.write_cache_enabled);

    Page none = GoodDisk();
    none[82] = 0x4000;
    none[85] = 0x4000;
    none[83] = 0x4000 | (1U << 10);
    id = Parse(none);
    CHECK(id.refusal == Refusal::None);
    CHECK_FALSE(id.write_cache_supported);
    CHECK_FALSE(id.write_cache_enabled);
    CHECK_FALSE(id.flush_cache_supported);
    CHECK_FALSE(id.flush_cache_ext_supported);

    Page plain = GoodDisk();
    plain[83] = 0x4000 | (1U << 12) | (1U << 10);
    id = Parse(plain);
    CHECK(id.flush_cache_supported);
    CHECK_FALSE(id.flush_cache_ext_supported);

    Page unvalidated = GoodDisk();
    unvalidated[82] = (1U << 5);
    unvalidated[85] = (1U << 5);
    id = Parse(unvalidated);
    CHECK_FALSE(id.write_cache_supported);
    CHECK_FALSE(id.write_cache_enabled);
}

TEST_CASE("ata T16: checksum good, bad and absent", "[ata]")
{
    Page good = GoodDisk();
    Seal(good);
    AtaIdentity id = Parse(good);
    CHECK(id.checksum_present);
    CHECK(id.refusal == Refusal::None);

    Page bad = good;
    bad[40] ^= 0x0100;
    id = Parse(bad);
    CHECK(id.checksum_present);
    CHECK(id.refusal == Refusal::Checksum);

    Page wrong_byte = good;
    wrong_byte[255] = static_cast<uint16_t>(wrong_byte[255] ^ 0x0100);
    CHECK(Parse(wrong_byte).refusal == Refusal::Checksum);

    // A word 255 whose low byte is not A5h carries no checksum, whatever its other bits say.
    Page absent = GoodDisk();
    absent[255] = 0x1234;
    id = Parse(absent);
    CHECK_FALSE(id.checksum_present);
    CHECK(id.refusal == Refusal::None);
}

TEST_CASE("ata T16: strings are byte-swapped and trimmed", "[ata]")
{
    const AtaIdentity id = Parse(GoodDisk());
    CHECK(std::string_view(id.model) == "QEMU HARDDISK");
    CHECK(std::string_view(id.serial) == "feather-sata0");
    CHECK(std::string_view(id.firmware) == "2.5+");

    Page page = GoodDisk();
    PutText(page, 27, 40, "  Padded Model  ");
    page[10] = 0x0000;
    PutText(page, 11, 18, "ab\x01z");
    const AtaIdentity padded = Parse(page);
    CHECK(std::string_view(padded.model) == "Padded Model");
    CHECK(std::string_view(padded.serial) == "ab?z");

    Page full = GoodDisk();
    PutText(full, 27, 40, "0123456789012345678901234567890123456789");
    const AtaIdentity filled = Parse(full);
    CHECK(std::string_view(filled.model) == "0123456789012345678901234567890123456789");
    CHECK(filled.model[40] == '\0');

    Page blank = GoodDisk();
    PutText(blank, 27, 40, "");
    CHECK(std::string_view(Parse(blank).model).empty());
}

TEST_CASE("ata T16: refusal texts", "[ata]")
{
    CHECK(std::string_view(RefusalText(Refusal::NoLba48)) == "48-bit LBA not supported");
    CHECK(std::string_view(RefusalText(Refusal::None)) == "accepted");
    CHECK(std::string_view(RefusalText(Refusal::Checksum)).find("checksum") !=
          std::string_view::npos);
}
