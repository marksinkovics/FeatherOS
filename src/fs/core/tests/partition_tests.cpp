// SPDX-License-Identifier: MIT OR Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include "common/little_endian.h"
#include "fs/partition.h"
#include "tests/memory_device.h"
#include "tests/partition_image.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>

#include <unistd.h>

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
using FeatherDisk::Tests::MbrEntry;
using FeatherDisk::Tests::MemoryDevice;
using FeatherDisk::Tests::SealGptHeader;
using FeatherDisk::Tests::SetMbrEntry;
using FeatherDisk::Tests::WriteEbr;
using FeatherDisk::Tests::WriteGpt;

constexpr std::uint8_t Fat16 = 0x06;
constexpr std::uint8_t Fat16Lba = 0x0E;
constexpr std::uint8_t Linux = 0x83;
constexpr std::uint8_t Ntfs = 0x07;
constexpr std::uint8_t Extended = 0x05;
constexpr std::uint8_t ExtendedLba = 0x0F;
constexpr std::uint8_t ExtendedLinux = 0x85;

std::vector<Range> Find(MemoryDevice& device, std::size_t capacity = 16)
{
    std::vector<Range> ranges(capacity);
    ranges.resize(FeatherDisk::Partition::Find(device, ranges));
    return ranges;
}

// "5:{1100,500} 6:{6100,600}": number, first sector and count of each listed partition.
std::string Listed(const std::vector<Range>& ranges)
{
    std::string text;
    for (const Range& range : ranges)
    {
        text += (text.empty() ? "" : " ") + std::to_string(range.number) + ":{" +
                std::to_string(range.first_sector) + "," + std::to_string(range.sector_count) + "}";
    }
    return text;
}

std::string ListedOn(MemoryDevice& device, std::size_t capacity = 16)
{
    return Listed(Find(device, capacity));
}

class CountingDevice final : public FeatherDisk::BlockDevice
{
public:
    explicit CountingDevice(MemoryDevice& device) : inner(device) {}

    std::uint32_t SectorSize() const override
    {
        return inner.SectorSize();
    }
    std::uint64_t SectorCount() const override
    {
        return inner.SectorCount();
    }
    FeatherDisk::Status Read(std::uint64_t first_sector, std::span<std::uint8_t> buffer) override
    {
        ++reads;
        return inner.Read(first_sector, buffer);
    }
    FeatherDisk::Status Write(std::uint64_t first_sector,
                              std::span<const std::uint8_t> data) override
    {
        return inner.Write(first_sector, data);
    }
    FeatherDisk::Status Flush() override
    {
        return inner.Flush();
    }

    std::size_t reads = 0;

private:
    MemoryDevice& inner;
};

// The container of most tests: sectors 1000 to 20999 of a 30000-sector disk.
constexpr std::uint32_t ContainerFirst = 1000;
constexpr std::uint32_t ContainerCount = 20000;

void SetContainer(MemoryDevice& device)
{
    SetMbrEntry(device, 0, Extended, ContainerFirst, ContainerCount);
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

TEST_CASE("the test disk's layout: three primaries, an extended partition and two logicals")
{
    MemoryDevice device(512, 339968);
    SetMbrEntry(device, 0, Linux, 2048, 260096);
    SetMbrEntry(device, 1, Fat16, 262144, 32768);
    SetMbrEntry(device, 2, Linux, 294912, 8192);
    SetMbrEntry(device, 3, Extended, 303104, 28672);
    WriteEbr(device, 303104, {Linux, 2048, 8192}, {Extended, 10240, 18432});
    WriteEbr(device, 313344, {Linux, 2048, 16384});
    REQUIRE(ListedOn(device) == "1:{2048,260096} 2:{262144,32768} 3:{294912,8192} "
                                "5:{305152,8192} 6:{315392,16384}");
}

TEST_CASE("a logical starts relative to its own record and a link relative to the container")
{
    MemoryDevice disk(512, 30000);
    SetContainer(disk);
    SetMbrEntry(disk, 1, Linux, 25000, 3000);
    WriteEbr(disk, 1000, {Linux, 100, 500}, {ExtendedLba, 3000, 10000});
    WriteEbr(disk, 4000, {Linux, 100, 600}, {ExtendedLinux, 9000, 5000});
    WriteEbr(disk, 10000, {Fat16, 300, 700});
    REQUIRE(ListedOn(disk) == "2:{25000,3000} 5:{1100,500} 6:{4100,600} 7:{10300,700}");
}

TEST_CASE("a chain that loops ends after the logicals before the loop, in a few reads")
{
    MemoryDevice disk(512, 30000);
    SetContainer(disk);
    const auto walk = [&disk](std::size_t capacity)
    {
        CountingDevice counted(disk);
        std::vector<Range> ranges(capacity);
        ranges.resize(FeatherDisk::Partition::Find(counted, ranges));
        return std::make_pair(Listed(ranges), counted.reads);
    };

    WriteEbr(disk, 1000, {Linux, 10, 100}, {Extended, 0, 100});
    std::string listed;
    std::size_t reads = 0;
    std::tie(listed, reads) = walk(16);
    REQUIRE(listed == "5:{1010,100}");
    REQUIRE(reads == 2);

    WriteEbr(disk, 1000, {Linux, 10, 100}, {Extended, 5000, 100});
    WriteEbr(disk, 6000, {Linux, 10, 200}, {Extended, 0, 100});
    std::tie(listed, reads) = walk(16);
    REQUIRE(listed == "5:{1010,100} 6:{6010,200}");
    REQUIRE(reads == 3);

    WriteEbr(disk, 6000, {Linux, 10, 200}, {Extended, 8000, 100});
    WriteEbr(disk, 9000, {Linux, 10, 300}, {Extended, 5000, 100});
    std::tie(listed, reads) = walk(16);
    REQUIRE(listed == "5:{1010,100} 6:{6010,200} 7:{9010,300}");
    REQUIRE(reads == 4);

    MemoryDevice long_disk(512, 100000);
    SetMbrEntry(long_disk, 0, Extended, 1000, 90000);
    for (std::uint32_t record = 0; record < 1000; ++record)
    {
        WriteEbr(long_disk, 1000 + record * 10, {Linux, 2, 5}, {Extended, (record + 1) * 10, 100});
    }
    for (const std::size_t capacity : {200U, 16U})
    {
        CountingDevice counted(long_disk);
        std::vector<Range> ranges(capacity);
        ranges.resize(FeatherDisk::Partition::Find(counted, ranges));
        REQUIRE(ranges.size() == std::min<std::size_t>(capacity, 64));
        REQUIRE(ranges.back().number == 4 + ranges.size());
        REQUIRE(ranges.back().first_sector == 1000 + (ranges.size() - 1) * 10 + 2);
        REQUIRE(counted.reads == 1 + std::min<std::size_t>(capacity, 64));
    }
}

TEST_CASE("a logical that is not valid is skipped and the next one is still found")
{
    struct Case
    {
        const char* name;
        MbrEntry logical;
        const char* listed;
    };
    // The container is 1000..20999 on a 30000-sector disk; the next record is at 6000.
    const Case cases[] = {
        {"past the container", {Linux, 20000, 10}, "6:{6100,300}"},
        {"across the container's end", {Linux, 19900, 200}, "6:{6100,300}"},
        {"past the disk", {Linux, 40000, 10}, "6:{6100,300}"},
        {"empty", {Linux, 10, 0}, "5:{6100,300}"},
        {"at its own record", {Linux, 0, 10}, "6:{6100,300}"},
        {"another type", {Ntfs, 10, 10}, "6:{6100,300}"},
        {"an extended type", {Extended, 10, 10}, "5:{6100,300}"},
    };
    for (const Case& test : cases)
    {
        CAPTURE(test.name);
        MemoryDevice disk(512, 30000);
        SetContainer(disk);
        WriteEbr(disk, 1000, test.logical, {Extended, 5000, 100});
        WriteEbr(disk, 6000, {Linux, 100, 300});
        REQUIRE(ListedOn(disk) == test.listed);
    }
}

TEST_CASE("a link that is not valid ends the chain and keeps what was found")
{
    struct Case
    {
        const char* name;
        MbrEntry link;
    };
    const Case cases[] = {
        {"outside the container", {Extended, 20000, 100}},
        {"of a filesystem type", {Linux, 5000, 100}},
        {"empty", {Extended, 5000, 0}},
        {"missing", {0, 0, 0}},
    };
    for (const Case& test : cases)
    {
        CAPTURE(test.name);
        MemoryDevice disk(512, 30000);
        SetContainer(disk);
        WriteEbr(disk, 1000, {Linux, 10, 100}, test.link);
        WriteEbr(disk, 6000, {Linux, 100, 300});
        REQUIRE(ListedOn(disk) == "5:{1010,100}");
    }

    MemoryDevice valid(512, 30000);
    SetContainer(valid);
    WriteEbr(valid, 1000, {Linux, 10, 100}, {Extended, 5000, 100});
    WriteEbr(valid, 6000, {Linux, 100, 300});
    REQUIRE(ListedOn(valid) == "5:{1010,100} 6:{6100,300}");

    MemoryDevice unsigned_record(512, 30000);
    SetContainer(unsigned_record);
    WriteEbr(unsigned_record, 1000, {Linux, 10, 100}, {Extended, 5000, 100});
    WriteEbr(unsigned_record, 6000, {Linux, 100, 300});
    unsigned_record.Sector(6000)[511] = 0;
    REQUIRE(ListedOn(unsigned_record) == "5:{1010,100}");

    MemoryDevice unreadable(512, 30000);
    SetContainer(unreadable);
    WriteEbr(unreadable, 1000, {Linux, 10, 100}, {Extended, 5000, 100});
    WriteEbr(unreadable, 6000, {Linux, 100, 300});
    unreadable.FailReadsAt(6000);
    REQUIRE(ListedOn(unreadable) == "5:{1010,100}");

    MemoryDevice no_first_record(512, 30000);
    SetContainer(no_first_record);
    SetMbrEntry(no_first_record, 1, Linux, 25000, 100);
    REQUIRE(ListedOn(no_first_record) == "2:{25000,100}");
}

TEST_CASE("numbers leave gaps for empty slots and stop at the caller's capacity")
{
    MemoryDevice gap(512, 30000);
    SetMbrEntry(gap, 0, Linux, 100, 100);
    SetMbrEntry(gap, 2, Fat16Lba, 300, 100);
    SetMbrEntry(gap, 3, Extended, 1000, 20000);
    WriteEbr(gap, 1000, {Linux, 10, 100});
    REQUIRE(ListedOn(gap) == "1:{100,100} 3:{300,100} 5:{1010,100}");

    MemoryDevice unsupported(512, 30000);
    SetMbrEntry(unsupported, 0, Ntfs, 100, 100);
    SetMbrEntry(unsupported, 1, Linux, 300, 100);
    SetMbrEntry(unsupported, 2, Extended, 1000, 20000);
    WriteEbr(unsupported, 1000, {Ntfs, 10, 100}, {Extended, 5000, 100});
    WriteEbr(unsupported, 6000, {Linux, 10, 100});
    REQUIRE(ListedOn(unsupported) == "2:{300,100} 6:{6010,100}");

    MemoryDevice disk(512, 70000);
    SetMbrEntry(disk, 0, Linux, 100, 100);
    SetMbrEntry(disk, 1, Fat16, 300, 100);
    SetMbrEntry(disk, 2, Fat16Lba, 500, 100);
    SetMbrEntry(disk, 3, Extended, 1000, 60000);
    for (std::uint32_t record = 0; record < 14; ++record)
    {
        const MbrEntry link =
            record < 13 ? MbrEntry{Extended, (record + 1) * 4000, 1000} : MbrEntry{};
        WriteEbr(disk, 1000 + record * 4000, {Linux, 8, 100}, link);
    }
    const std::vector<Range> all = Find(disk);
    REQUIRE(all.size() == 16);
    REQUIRE(Listed(std::vector<Range>(all.begin(), all.begin() + 5)) ==
            "1:{100,100} 2:{300,100} 3:{500,100} 5:{1008,100} 6:{5008,100}");
    REQUIRE(all.back().number == 17);
    REQUIRE(all.back().first_sector == 49008);
    REQUIRE(Listed(Find(disk, 4)) == "1:{100,100} 2:{300,100} 3:{500,100} 5:{1008,100}");
    REQUIRE(Find(disk, 0).empty());
    REQUIRE(Find(disk, 3).size() == 3);
}

TEST_CASE("only the first extended entry is walked")
{
    MemoryDevice disk(512, 60000);
    SetMbrEntry(disk, 0, Linux, 100, 100);
    SetMbrEntry(disk, 1, Extended, 1000, 20000);
    SetMbrEntry(disk, 2, ExtendedLba, 30000, 10000);
    SetMbrEntry(disk, 3, ExtendedLinux, 45000, 10000);
    WriteEbr(disk, 1000, {Linux, 10, 100});
    WriteEbr(disk, 30000, {Linux, 10, 100});
    WriteEbr(disk, 45000, {Linux, 10, 100});
    REQUIRE(ListedOn(disk) == "1:{100,100} 5:{1010,100}");

    MemoryDevice unusable_first(512, 60000);
    SetMbrEntry(unusable_first, 0, Extended, 1000, 0);
    SetMbrEntry(unusable_first, 1, ExtendedLba, 30000, 10000);
    WriteEbr(unusable_first, 1000, {Linux, 10, 100});
    WriteEbr(unusable_first, 30000, {Linux, 10, 100});
    REQUIRE(ListedOn(unusable_first).empty());

    MemoryDevice outside_disk(512, 60000);
    SetMbrEntry(outside_disk, 0, Extended, 59000, 5000);
    SetMbrEntry(outside_disk, 1, Linux, 100, 100);
    WriteEbr(outside_disk, 59000, {Linux, 10, 100});
    REQUIRE(ListedOn(outside_disk) == "2:{100,100}");
}

TEST_CASE("a protective MBR's extended entry is not walked and GPT entries are numbered by index")
{
    MemoryDevice device(512, 8192);
    WriteGpt(device, {{0, 64, 1063}, {2, 3000, 3999}, {5, 5000, 5999}});
    SetMbrEntry(device, 1, Extended, 6000, 1000);
    WriteEbr(device, 6000, {Linux, 10, 100});
    REQUIRE(ListedOn(device) == "1:{64,1000} 3:{3000,1000} 6:{5000,1000}");

    MemoryDevice large_sectors(4096, 4000);
    SetMbrEntry(large_sectors, 0, Linux, 10, 20);
    SetMbrEntry(large_sectors, 1, Extended, 100, 2000);
    WriteEbr(large_sectors, 100, {Linux, 8, 100}, {Extended, 500, 100});
    WriteEbr(large_sectors, 600, {Fat16, 8, 200});
    REQUIRE(ListedOn(large_sectors) == "1:{10,20} 5:{108,100} 6:{608,200}");
}

namespace
{

std::string RunCapturing(const std::string& command)
{
    std::string output;
    if (FILE* pipe = popen(command.c_str(), "r"))
    {
        char chunk[512];
        while (std::fgets(chunk, sizeof(chunk), pipe) != nullptr)
        {
            output += chunk;
        }
        pclose(pipe);
    }
    return output;
}

std::uint64_t NumberAfter(const std::string& line, const std::string& key)
{
    const std::size_t at = line.find(key);
    return at == std::string::npos ? 0 : std::strtoull(line.c_str() + at + key.size(), nullptr, 10);
}

} // namespace

TEST_CASE("the partitions found match those sfdisk writes and reports")
{
    if (std::system("command -v sfdisk >/dev/null 2>&1") != 0)
    {
        SKIP("sfdisk is not installed");
    }
    constexpr std::uint64_t Sectors = 40960;
    const std::filesystem::path folder =
        std::filesystem::temp_directory_path() / ("featherdisk-sfdisk-" + std::to_string(getpid()));
    std::filesystem::create_directories(folder);
    const std::string image = (folder / "disk.img").string();
    {
        std::ofstream file(image, std::ios::binary);
        file.seekp(Sectors * 512 - 1);
        file.put('\0');
    }
    const std::string script = "label: dos\n"
                               "start=2048, size=8192, type=83\n"
                               "start=10240, size=4096, type=6\n"
                               "start=14336, size=2048, type=83\n"
                               "start=16384, size=20480, type=5\n"
                               "start=18432, size=4096, type=83\n"
                               "start=24576, size=8192, type=83\n"
                               "start=34816, size=2048, type=e\n";
    std::ofstream(folder / "script") << script;
    const std::string quoted = "'" + image + "'";
    const std::string created =
        RunCapturing("sfdisk -q " + quoted + " < '" + (folder / "script").string() + "' 2>&1");
    const std::string json = RunCapturing("sfdisk -J " + quoted + " 2>&1");

    std::map<std::uint32_t, std::pair<std::uint64_t, std::uint64_t>> expected;
    std::istringstream lines(json);
    for (std::string line; std::getline(lines, line);)
    {
        const std::size_t node = line.find("\"node\"");
        const std::size_t type = line.find("\"type\": \"");
        if (node == std::string::npos || type == std::string::npos)
        {
            continue;
        }
        const std::string kind = line.substr(type + 9, line.find('"', type + 9) - type - 9);
        if (kind == "5" || kind == "f" || kind == "85")
        {
            continue;
        }
        const std::size_t end = line.find('"', node + 9 + 1);
        const std::string name = line.substr(node + 9 + 1, end - node - 10);
        const std::size_t digits = name.find_last_not_of("0123456789") + 1;
        expected[static_cast<std::uint32_t>(std::stoul(name.substr(digits)))] = {
            NumberAfter(line, "\"start\":"), NumberAfter(line, "\"size\":")};
    }

    MemoryDevice device(512, Sectors);
    {
        std::ifstream file(image, std::ios::binary);
        file.read(reinterpret_cast<char*>(device.Bytes().data()),
                  static_cast<std::streamsize>(device.Bytes().size()));
    }
    std::filesystem::remove_all(folder);

    REQUIRE_FALSE(expected.empty());
    CAPTURE(created, json);
    std::map<std::uint32_t, std::pair<std::uint64_t, std::uint64_t>> found;
    for (const Range& range : Find(device))
    {
        found[range.number] = {range.first_sector, range.sector_count};
    }
    REQUIRE(found == expected);
}
