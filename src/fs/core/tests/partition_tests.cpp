// SPDX-License-Identifier: MIT OR Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include "common/little_endian.h"
#include "fs/partition.h"
#include "tests/memory_device.h"

#include <algorithm>
#include <cstring>
#include <vector>

namespace
{

using FeatherDisk::LoadLittleEndian;
using FeatherDisk::StoreLittleEndian;
using FeatherDisk::Partition::Range;
using FeatherDisk::Tests::MemoryDevice;

constexpr std::uint8_t Fat16 = 0x06;
constexpr std::uint8_t Fat16Lba = 0x0E;
constexpr std::uint8_t Linux = 0x83;

void SetMbrEntry(MemoryDevice& device, std::size_t index, std::uint8_t type, std::uint32_t first,
                 std::uint32_t count)
{
    std::uint8_t* entry = device.Sector(0).data() + 446 + index * 16;
    entry[4] = type;
    StoreLittleEndian(entry + 8, first);
    StoreLittleEndian(entry + 12, count);
    device.Sector(0)[510] = 0x55;
    device.Sector(0)[511] = 0xAA;
}

// Written independently of the code under test: bit by bit, without its masking trick.
std::uint32_t Crc32(const std::uint8_t* bytes, std::size_t length)
{
    std::uint32_t crc = 0xFFFFFFFFU;
    for (std::size_t index = 0; index < length; ++index)
    {
        crc ^= bytes[index];
        for (int bit = 0; bit < 8; ++bit)
        {
            crc = (crc >> 1) ^ ((crc & 1U) != 0 ? 0xEDB88320U : 0U);
        }
    }
    return ~crc;
}

struct GptEntry
{
    std::size_t index;
    std::uint64_t first;
    std::uint64_t last;
};

struct GptLayout
{
    std::uint32_t entry_count = 128;
    std::uint32_t entry_size = 128;
};

std::uint8_t* GptHeader(MemoryDevice& device)
{
    return device.Sector(1).data();
}

void SealGptHeader(MemoryDevice& device)
{
    std::uint8_t* header = GptHeader(device);
    StoreLittleEndian<std::uint32_t>(header + 16, 0);
    StoreLittleEndian(header + 16, Crc32(header, LoadLittleEndian<std::uint32_t>(header + 12)));
}

// A protective MBR, the GPT header in sector 1 and its entry array from sector 2. Entries not
// listed are unused (an all-zero type GUID).
void WriteGpt(MemoryDevice& device, const std::vector<GptEntry>& entries, GptLayout layout = {})
{
    SetMbrEntry(device, 0, 0xEE, 1, 0xFFFFFFFFU);

    std::vector<std::uint8_t> array(static_cast<std::size_t>(layout.entry_count) *
                                    layout.entry_size);
    for (const GptEntry& entry : entries)
    {
        std::uint8_t* raw = array.data() + entry.index * layout.entry_size;
        raw[0] = 0xA2;
        raw[15] = 0x77;
        StoreLittleEndian(raw + 32, entry.first);
        StoreLittleEndian(raw + 40, entry.last);
    }
    const std::uint32_t sector_size = device.SectorSize();
    for (std::size_t offset = 0; offset < array.size(); offset += sector_size)
    {
        const std::size_t length = std::min<std::size_t>(sector_size, array.size() - offset);
        std::memcpy(device.Sector(2 + offset / sector_size).data(), array.data() + offset, length);
    }

    std::uint8_t* header = GptHeader(device);
    std::memcpy(header, "EFI PART", 8);
    StoreLittleEndian<std::uint32_t>(header + 8, 0x00010000);
    StoreLittleEndian<std::uint32_t>(header + 12, 92);
    StoreLittleEndian<std::uint64_t>(header + 24, 1);
    StoreLittleEndian<std::uint64_t>(header + 32, device.SectorCount() - 1);
    StoreLittleEndian<std::uint64_t>(header + 72, 2);
    StoreLittleEndian(header + 80, layout.entry_count);
    StoreLittleEndian(header + 84, layout.entry_size);
    StoreLittleEndian(header + 88, Crc32(array.data(), array.size()));
    SealGptHeader(device);
}

std::vector<Range> Find(MemoryDevice& device, std::size_t capacity = 16)
{
    std::vector<Range> ranges(capacity);
    ranges.resize(FeatherDisk::Partition::Find(device, ranges));
    return ranges;
}

} // namespace

namespace FeatherDisk::Partition
{

bool operator==(const Range& left, const Range& right)
{
    return left.first_sector == right.first_sector && left.sector_count == right.sector_count;
}

} // namespace FeatherDisk::Partition

TEST_CASE("the MBR's FAT16 entries are found in table order and other types are skipped")
{
    MemoryDevice device(512, 4096);
    SetMbrEntry(device, 0, Linux, 100, 50);
    SetMbrEntry(device, 1, Fat16Lba, 200, 100);
    SetMbrEntry(device, 2, Fat16, 2048, 1000);
    REQUIRE(Find(device) == std::vector<Range>{{200, 100}, {2048, 1000}});
}

TEST_CASE("MBR entries that start at sector 0, are empty or leave the disk are skipped")
{
    MemoryDevice device(512, 4096);
    SetMbrEntry(device, 0, Fat16, 0, 10);
    SetMbrEntry(device, 1, Fat16, 10, 0);
    SetMbrEntry(device, 2, Fat16, 4000, 97);
    SetMbrEntry(device, 3, Fat16, 4000, 96);
    REQUIRE(Find(device) == std::vector<Range>{{4000, 96}});
}

TEST_CASE("a disk without the MBR signature has no partitions")
{
    MemoryDevice device(512, 4096);
    SetMbrEntry(device, 0, Fat16, 1, 100);
    device.Sector(0)[511] = 0;
    REQUIRE(Find(device).empty());
}

TEST_CASE("partitions beyond the caller's capacity are dropped")
{
    MemoryDevice mbr_disk(512, 4096);
    SetMbrEntry(mbr_disk, 0, Fat16, 1, 100);
    SetMbrEntry(mbr_disk, 1, Fat16, 101, 100);
    REQUIRE(Find(mbr_disk, 1) == std::vector<Range>{{1, 100}});

    MemoryDevice gpt_disk(512, 8192);
    std::vector<GptEntry> entries;
    for (std::size_t index = 0; index < 20; ++index)
    {
        entries.push_back({index, 100 + index * 100, 199 + index * 100});
    }
    WriteGpt(gpt_disk, entries);
    const std::vector<Range> found = Find(gpt_disk);
    REQUIRE(found.size() == 16);
    REQUIRE(found[15] == Range{1600, 100});
    REQUIRE(Find(gpt_disk, 0).empty());
}

TEST_CASE("a protective MBR selects the GUID partition table's used entries")
{
    MemoryDevice device(512, 4096);
    WriteGpt(device, {{0, 64, 1063}, {2, 3000, 4096}, {5, 2000, 2999}});
    // Ignored: the protective entry hands everything to the GPT.
    SetMbrEntry(device, 1, Fat16, 64, 100);
    REQUIRE(Find(device) == std::vector<Range>{{64, 1000}, {2000, 1000}});
}

TEST_CASE("a GUID partition table with a bad header or entry CRC mounts nothing")
{
    MemoryDevice device(512, 4096);
    WriteGpt(device, {{0, 64, 1063}});
    REQUIRE(Find(device).size() == 1);

    GptHeader(device)[40] ^= 1;
    REQUIRE(Find(device).empty());

    WriteGpt(device, {{0, 64, 1063}});
    device.Sector(2)[100] ^= 1;
    REQUIRE(Find(device).empty());

    WriteGpt(device, {{0, 64, 1063}});
    std::memcpy(GptHeader(device), "EFI PARX", 8);
    REQUIRE(Find(device).empty());
}

TEST_CASE("GPT headers with impossible fields are refused even with a valid CRC")
{
    MemoryDevice device(512, 4096);
    const auto refused = [&device](std::size_t offset, auto value)
    {
        WriteGpt(device, {{0, 64, 1063}});
        StoreLittleEndian(GptHeader(device) + offset, value);
        SealGptHeader(device);
        return Find(device).empty();
    };
    REQUIRE(refused(24, std::uint64_t{2}));
    REQUIRE(refused(72, std::uint64_t{4090}));
    REQUIRE(refused(72, std::uint64_t{0}));
    REQUIRE(refused(80, std::uint32_t{1025}));
    REQUIRE(refused(84, std::uint32_t{96}));
    REQUIRE(refused(84, std::uint32_t{384}));
    REQUIRE(refused(84, std::uint32_t{1024}));

    WriteGpt(device, {{0, 64, 1063}});
    StoreLittleEndian<std::uint32_t>(GptHeader(device) + 12, 91);
    SealGptHeader(device);
    REQUIRE(Find(device).empty());
}

TEST_CASE("GPT entries may be larger than 128 bytes")
{
    MemoryDevice device(512, 4096);
    WriteGpt(device, {{1, 100, 199}, {3, 300, 399}}, {.entry_count = 128, .entry_size = 256});
    REQUIRE(Find(device) == std::vector<Range>{{100, 100}, {300, 100}});
}

TEST_CASE("4096-byte sectors hold the MBR, the GPT header and 32 entries each")
{
    MemoryDevice mbr_disk(4096, 1024);
    SetMbrEntry(mbr_disk, 0, Fat16, 8, 512);
    REQUIRE(Find(mbr_disk) == std::vector<Range>{{8, 512}});

    MemoryDevice gpt_disk(4096, 1024);
    WriteGpt(gpt_disk, {{0, 8, 263}, {40, 300, 999}});
    REQUIRE(Find(gpt_disk) == std::vector<Range>{{8, 256}, {300, 700}});
}

TEST_CASE("read errors and unsupported sector sizes find no partitions")
{
    MemoryDevice device(512, 4096);
    WriteGpt(device, {{0, 64, 1063}});
    device.FailReadsFrom(5);
    REQUIRE(Find(device).empty());
    device.FailReadsFrom(1);
    REQUIRE(Find(device).empty());
    device.FailReadsFrom(0);
    REQUIRE(Find(device).empty());

    MemoryDevice small_sectors(256, 4096);
    REQUIRE(Find(small_sectors).empty());
    MemoryDevice odd_sectors(768, 4096);
    SetMbrEntry(odd_sectors, 0, Fat16, 1, 100);
    REQUIRE(Find(odd_sectors).empty());
}
