// SPDX-License-Identifier: MIT OR Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include "common/little_endian.h"
#include "fs/partition.h"
#include "tests/memory_device.h"
#include "tests/partition_image.h"

#include <cstring>
#include <vector>

namespace
{

using FeatherDisk::LoadLittleEndian;
using FeatherDisk::StoreLittleEndian;
using FeatherDisk::Partition::Identify;
using FeatherDisk::Partition::Range;
using FeatherDisk::Partition::Scheme;
using FeatherDisk::Tests::GptEntry;
using FeatherDisk::Tests::GptHeader;
using FeatherDisk::Tests::LinuxFilesystemType;
using FeatherDisk::Tests::MemoryDevice;
using FeatherDisk::Tests::SealGptHeader;
using FeatherDisk::Tests::SetMbrEntry;
using FeatherDisk::Tests::WriteGpt;

constexpr std::uint8_t Fat16 = 0x06;
constexpr std::uint8_t Fat16Lba = 0x0E;
constexpr std::uint8_t Linux = 0x83;
constexpr std::uint8_t Ntfs = 0x07;

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

TEST_CASE("the MBR's FAT16 and Linux entries are found in table order and other types skipped")
{
    MemoryDevice device(512, 4096);
    SetMbrEntry(device, 0, Ntfs, 10, 50);
    SetMbrEntry(device, 1, Fat16Lba, 200, 100);
    SetMbrEntry(device, 2, Linux, 2048, 1000);
    SetMbrEntry(device, 3, Fat16, 100, 50);
    REQUIRE(Find(device) == std::vector<Range>{{200, 100}, {2048, 1000}, {100, 50}});
}

TEST_CASE("a GUID partition table's Linux filesystem entries are found")
{
    MemoryDevice device(512, 4096);
    WriteGpt(device, {{0, 64, 1063, LinuxFilesystemType}, {1, 2000, 2999}});
    REQUIRE(Find(device) == std::vector<Range>{{64, 1000}, {2000, 1000}});
    REQUIRE(Identify(device) == Scheme::Gpt);
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

TEST_CASE("the scheme is the one sector 0 announces")
{
    MemoryDevice blank(512, 4096);
    REQUIRE(Identify(blank) == Scheme::None);

    MemoryDevice mbr_disk(512, 4096);
    SetMbrEntry(mbr_disk, 0, Linux, 100, 50);
    REQUIRE(Identify(mbr_disk) == Scheme::Mbr);

    MemoryDevice gpt_disk(512, 4096);
    WriteGpt(gpt_disk, {{0, 64, 1063}});
    REQUIRE(Identify(gpt_disk) == Scheme::Gpt);
    // A damaged GPT is still a GPT; it only yields no partitions.
    GptHeader(gpt_disk)[40] ^= 1;
    REQUIRE(Identify(gpt_disk) == Scheme::Gpt);

    gpt_disk.FailReadsFrom(0);
    REQUIRE(Identify(gpt_disk) == Scheme::None);
    MemoryDevice odd_sectors(768, 4096);
    SetMbrEntry(odd_sectors, 0, Fat16, 1, 100);
    REQUIRE(Identify(odd_sectors) == Scheme::None);
}
