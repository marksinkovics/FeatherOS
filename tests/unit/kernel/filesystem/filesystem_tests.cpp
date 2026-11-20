#include <catch2/catch_test_macros.hpp>

#include "kernel/files.h"
#include "kernel/filesystem/drivers.h"
#include "kernel/filesystem/initramfs.h"
#include "kernel/filesystem/procfs.h"
#include "kernel/filesystem/tmpfs.h"
#include "kernel/filesystem/vfs.h"
#include "kernel/storage/block_device.h"

#include "support/heap_failure.h"

#include "fs/block_device.h"
#include "fs/ext2.h"
#include "fs/fat.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <set>
#include <span>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace
{

// As the kernel does at boot.
[[maybe_unused]] const bool built_in_mounted = FeatherOS::VFS::MountBuiltIn();

constexpr uint32_t DiskSectorCount = 5001;
constexpr uint32_t PartitionStart = 1;
constexpr uint32_t PartitionSectorCount = 5000;
constexpr uint32_t FatSizeSectors = 20;
constexpr uint32_t RootDirectorySectors = 1;
constexpr uint32_t RootDirectorySector = PartitionStart + 1 + FatSizeSectors;
constexpr uint32_t DataStartSector = RootDirectorySector + RootDirectorySectors;

std::vector<std::array<uint8_t, FeatherOS::BlockDevice::SectorSize>> disk;
uint32_t flush_count = 0;
bool writes_fail = false;
bool reads_fail = false;
std::function<void(uint64_t, const void*)> on_write;

void WriteLittleEndian16(uint8_t* destination, uint16_t value)
{
    destination[0] = static_cast<uint8_t>(value);
    destination[1] = static_cast<uint8_t>(value >> 8);
}

void WriteLittleEndian32(uint8_t* destination, uint32_t value)
{
    destination[0] = static_cast<uint8_t>(value);
    destination[1] = static_cast<uint8_t>(value >> 8);
    destination[2] = static_cast<uint8_t>(value >> 16);
    destination[3] = static_cast<uint8_t>(value >> 24);
}

void AddRootFile(const std::string& contents)
{
    uint8_t* root = disk[RootDirectorySector].data();
    const char short_name[] = "BLOCK   TXT";
    std::memcpy(root, short_name, sizeof(short_name) - 1);
    root[11] = 0x20;
    WriteLittleEndian16(root + 26, 2);
    WriteLittleEndian32(root + 28, static_cast<uint32_t>(contents.size()));

    const uint32_t first_cluster_sector = DataStartSector;
    const uint32_t second_cluster_sector = DataStartSector + 1;
    const size_t first_chunk = contents.size() < FeatherOS::BlockDevice::SectorSize
                                   ? contents.size()
                                   : FeatherOS::BlockDevice::SectorSize;
    std::memcpy(disk[first_cluster_sector].data(), contents.data(), first_chunk);
    if (contents.size() > first_chunk)
    {
        std::memcpy(disk[second_cluster_sector].data(), contents.data() + first_chunk,
                    contents.size() - first_chunk);
    }

    uint8_t* fat = disk[PartitionStart + 1].data();
    WriteLittleEndian16(fat + 4, contents.size() > first_chunk ? 3 : 0xFFF8);
    WriteLittleEndian16(fat + 6, 0xFFF8);
}

void FormatFat16(uint32_t root_directory_sectors)
{
    disk.assign(DiskSectorCount, {});

    uint8_t* mbr = disk[0].data();
    uint8_t* partition = mbr + 446;
    partition[4] = 0x06;
    WriteLittleEndian32(partition + 8, PartitionStart);
    WriteLittleEndian32(partition + 12, PartitionSectorCount);
    mbr[510] = 0x55;
    mbr[511] = 0xAA;

    uint8_t* boot = disk[PartitionStart].data();
    WriteLittleEndian16(boot + 11, 512);
    boot[13] = 1;
    WriteLittleEndian16(boot + 14, 1);
    boot[16] = 1;
    WriteLittleEndian16(boot + 17, static_cast<uint16_t>(root_directory_sectors * 16));
    WriteLittleEndian16(boot + 19, PartitionSectorCount);
    boot[21] = 0xF8;
    WriteLittleEndian16(boot + 22, FatSizeSectors);
    boot[510] = 0x55;
    boot[511] = 0xAA;
}

void MakeFat16Disk(const std::string& contents)
{
    FormatFat16(RootDirectorySectors);
    AddRootFile(contents);
}

// Files /F0000.TXT, /F0001.TXT and so on, of two bytes each ('A' + n % 26, 'a' + n % 26), in a root
// directory that holds them all.
constexpr uint32_t ManyFilesCount = 1025;
constexpr uint32_t ManyFilesRootSectors = 80;

std::string ManyFileName(uint32_t number)
{
    char name[16];
    std::snprintf(name, sizeof(name), "/F%04u.TXT", number);
    return name;
}

void MakeManyFilesDisk()
{
    FormatFat16(ManyFilesRootSectors);
    const uint32_t root_sector = PartitionStart + 1 + FatSizeSectors;
    const uint32_t data_sector = root_sector + ManyFilesRootSectors;
    uint8_t* fat = disk[PartitionStart + 1].data();
    for (uint32_t number = 0; number < ManyFilesCount; ++number)
    {
        uint8_t* entry = disk[root_sector + number / 16].data() + (number % 16) * 32;
        char short_name[12];
        std::snprintf(short_name, sizeof(short_name), "F%04u   TXT", number);
        std::memcpy(entry, short_name, 11);
        entry[11] = 0x20;
        const uint32_t cluster = 2 + number;
        WriteLittleEndian16(entry + 26, static_cast<uint16_t>(cluster));
        WriteLittleEndian32(entry + 28, 2);
        WriteLittleEndian16(fat + cluster * 2, 0xFFF8);
        uint8_t* data = disk[data_sector + number].data();
        data[0] = static_cast<uint8_t>('A' + number % 26);
        data[1] = static_cast<uint8_t>('a' + number % 26);
    }
}

void WriteOctal(char* field, size_t field_size, uint64_t value)
{
    std::memset(field, '0', field_size);
    field[field_size - 1] = '\0';
    for (size_t index = field_size - 2; index > 0; --index)
    {
        field[index] = static_cast<char>('0' + (value & 7));
        value >>= 3;
    }
}

void AppendTarFile(std::vector<uint8_t>* archive, const std::string& path,
                   const std::string& contents)
{
    const size_t offset = archive->size();
    archive->resize(offset + 512 + (contents.size() + 511) / 512 * 512, 0);
    auto* header = reinterpret_cast<char*>(archive->data() + offset);
    std::memcpy(header, path.c_str(), path.size());
    WriteOctal(header + 100, 8, 0644);
    WriteOctal(header + 108, 8, 0);
    WriteOctal(header + 116, 8, 0);
    WriteOctal(header + 124, 12, contents.size());
    WriteOctal(header + 136, 12, 0);
    std::memset(header + 148, ' ', 8);
    header[156] = '0';
    std::memcpy(header + 257, "ustar", 5);
    std::memcpy(header + 263, "00", 2);
    uint64_t checksum = 0;
    for (size_t index = 0; index < 512; ++index)
    {
        checksum += static_cast<uint8_t>(header[index]);
    }
    std::snprintf(header + 148, 8, "%06llo", static_cast<unsigned long long>(checksum));
    header[154] = '\0';
    header[155] = ' ';
    std::memcpy(archive->data() + offset + 512, contents.data(), contents.size());
}

std::vector<uint8_t> MakeTarArchive(const std::vector<std::pair<std::string, std::string>>& files)
{
    std::vector<uint8_t> archive;
    for (const auto& [path, contents] : files)
    {
        AppendTarFile(&archive, path, contents);
    }
    archive.resize(archive.size() + 2 * 512, 0);
    return archive;
}

std::vector<uint8_t> MakeTarArchive(const std::string& path, const std::string& contents)
{
    return MakeTarArchive({{path, contents}});
}

} // namespace

namespace FeatherOS::BlockDevice
{

uint64_t SectorCount()
{
    return disk.size();
}

bool ReadSector(uint64_t sector, void* buffer)
{
    if (buffer == nullptr || sector >= disk.size() || reads_fail)
    {
        return false;
    }
    std::memcpy(buffer, disk[sector].data(), SectorSize);
    return true;
}

bool WriteSector(uint64_t sector, const void* buffer)
{
    if (buffer == nullptr || sector >= disk.size() || writes_fail)
    {
        return false;
    }
    if (on_write)
    {
        on_write(sector, buffer);
    }
    std::memcpy(disk[sector].data(), buffer, SectorSize);
    return true;
}

bool Flush()
{
    ++flush_count;
    return true;
}

} // namespace FeatherOS::BlockDevice

TEST_CASE("USTAR initramfs exposes file contents")
{
    const std::string contents = "FeatherOS unit test archive";
    const std::vector<uint8_t> archive = MakeTarArchive("system/hello.txt", contents);
    REQUIRE(FeatherOS::Initramfs::Mount(archive.data(), archive.size()));

    FeatherOS::VFS::FileHandle file{};
    REQUIRE(FeatherOS::VFS::Open("hello.txt", &file));
    std::vector<char> output(contents.size());
    uint64_t bytes_read = 0;
    REQUIRE(FeatherOS::VFS::Read(&file, output.data(), output.size(), &bytes_read));
    REQUIRE(bytes_read == contents.size());
    REQUIRE(std::string(output.begin(), output.end()) == contents);
    FeatherOS::VFS::Close(&file);
}

TEST_CASE("USTAR initramfs directories are separate mounts, with etc copied to /etc")
{
    const std::vector<uint8_t> archive = MakeTarArchive({{"system/ls", "program"},
                                                         {"system/nested/deep", "hidden"},
                                                         {"etc/hosts", "127.0.0.1 localhost\n"},
                                                         {"etc/empty", ""},
                                                         {"stray", "outside"}});
    REQUIRE(FeatherOS::Initramfs::Mount(archive.data(), archive.size()));
    FeatherOS::Tmpfs::Reset();
    REQUIRE(FeatherOS::VFS::LoadEtc());

    FeatherOS::VFS::FileInfo info{};
    REQUIRE(FeatherOS::VFS::Stat("/etc/hosts", &info));
    REQUIRE(info.size == 20);
    REQUIRE(FeatherOS::VFS::Stat("/system/ls", &info));
    REQUIRE(FeatherOS::VFS::Stat("ls", &info));
    REQUIRE_FALSE(FeatherOS::VFS::Stat("/system/hosts", &info));
    REQUIRE_FALSE(FeatherOS::VFS::Stat("/etc/ls", &info));
    REQUIRE_FALSE(FeatherOS::VFS::Stat("stray", &info));
    REQUIRE(FeatherOS::VFS::IsDirectory("/etc"));
    REQUIRE(FeatherOS::VFS::IsReadOnly("/system/ls"));
    REQUIRE_FALSE(FeatherOS::VFS::IsReadOnly("/etc/hosts"));

    FeatherOS::Filesystem::DirectoryEntry entry{};
    REQUIRE(FeatherOS::VFS::ReadDirectory("/system", 0, &entry));
    REQUIRE(std::string(entry.name) == "ls");
    REQUIRE_FALSE(FeatherOS::VFS::ReadDirectory("/system", 1, &entry));
    REQUIRE(FeatherOS::VFS::ReadDirectory("/etc", 0, &entry));
    REQUIRE(std::string(entry.name) == "hosts");
    REQUIRE(entry.size == 20);
    REQUIRE(FeatherOS::VFS::ReadDirectory("/etc", 1, &entry));
    REQUIRE(std::string(entry.name) == "empty");
    REQUIRE(entry.size == 0);
    REQUIRE_FALSE(FeatherOS::VFS::ReadDirectory("/etc", 2, &entry));

    // A change lasts until the next boot copies the archive's file again.
    FeatherOS::VFS::FileHandle file{};
    uint64_t count = 0;
    REQUIRE(FeatherOS::VFS::Open("/etc/hosts", &file));
    REQUIRE(FeatherOS::VFS::Truncate(&file, 0));
    REQUIRE(FeatherOS::VFS::Write(&file, "::1 me\n", 7, &count));
    FeatherOS::VFS::Close(&file);
    REQUIRE(FeatherOS::VFS::Stat("/etc/hosts", &info));
    REQUIRE(info.size == 7);
    REQUIRE(FeatherOS::VFS::LoadEtc());
    REQUIRE(FeatherOS::VFS::Stat("/etc/hosts", &info));
    REQUIRE(info.size == 20);
}

TEST_CASE("USTAR initramfs rejects a corrupt header checksum")
{
    std::vector<uint8_t> archive = MakeTarArchive("system/hello.txt", "contents");
    archive[0] ^= 1;
    REQUIRE(FeatherOS::Initramfs::Mount(archive.data(), archive.size()));
    FeatherOS::Initramfs::File file{};
    REQUIRE_FALSE(FeatherOS::Initramfs::Open("system", "hello.txt", &file));
}

TEST_CASE("USTAR initramfs rejects a truncated file payload")
{
    std::vector<uint8_t> archive = MakeTarArchive("system/hello.txt", std::string(700, 'x'));
    archive.resize(2 * 512);
    REQUIRE(FeatherOS::Initramfs::Mount(archive.data(), archive.size()));
    FeatherOS::Initramfs::File file{};
    REQUIRE_FALSE(FeatherOS::Initramfs::Open("system", "hello.txt", &file));
}

TEST_CASE("FAT16 VFS reads files across cluster boundaries")
{
    std::string contents(700, '\0');
    for (size_t index = 0; index < contents.size(); ++index)
    {
        contents[index] = static_cast<char>('A' + index % 26);
    }
    MakeFat16Disk(contents);
    REQUIRE(FeatherOS::VFS::MountBlockDevice());

    FeatherOS::VFS::FileInfo info{};
    REQUIRE(FeatherOS::VFS::Stat("/BLOCK.TXT", &info));
    REQUIRE(info.size == contents.size());

    FeatherOS::VFS::FileHandle file{};
    REQUIRE(FeatherOS::VFS::Open("/BLOCK.TXT", &file));
    std::vector<char> output(contents.size());
    uint64_t bytes_read = 0;
    REQUIRE(FeatherOS::VFS::Read(&file, output.data(), output.size(), &bytes_read));
    REQUIRE(bytes_read == contents.size());
    REQUIRE(std::string(output.begin(), output.end()) == contents);

    REQUIRE(FeatherOS::VFS::Seek(&file, 500));
    char boundary[32];
    REQUIRE(FeatherOS::VFS::Read(&file, boundary, sizeof(boundary), &bytes_read));
    REQUIRE(bytes_read == sizeof(boundary));
    REQUIRE(std::string(boundary, sizeof(boundary)) == contents.substr(500, sizeof(boundary)));

    FeatherOS::VFS::Close(&file);
    REQUIRE_FALSE(FeatherOS::VFS::Read(&file, boundary, sizeof(boundary), &bytes_read));
}

TEST_CASE("FAT16 mount rejects a malformed MBR signature")
{
    MakeFat16Disk("contents");
    disk[0][510] = 0;
    REQUIRE_FALSE(FeatherOS::VFS::MountBlockDevice());
}

TEST_CASE("FAT16 mount rejects unsupported bytes per sector")
{
    MakeFat16Disk("contents");
    WriteLittleEndian16(disk[PartitionStart].data() + 11, 1024);
    REQUIRE_FALSE(FeatherOS::VFS::MountBlockDevice());
}

TEST_CASE("FAT16 VFS reports missing files")
{
    MakeFat16Disk("contents");
    REQUIRE(FeatherOS::VFS::MountBlockDevice());
    FeatherOS::VFS::FileInfo info{};
    REQUIRE_FALSE(FeatherOS::VFS::Stat("/MISSING.TXT", &info));
}

TEST_CASE("FAT16 VFS rejects corrupt cluster chains")
{
    MakeFat16Disk(std::string(700, 'x'));
    WriteLittleEndian16(disk[PartitionStart + 1].data() + 4, 0xFFF7);
    REQUIRE(FeatherOS::VFS::MountBlockDevice());
    FeatherOS::VFS::FileHandle file{};
    REQUIRE(FeatherOS::VFS::Open("/BLOCK.TXT", &file));
    std::array<char, 700> contents{};
    uint64_t bytes_read = 0;
    REQUIRE_FALSE(FeatherOS::VFS::Read(&file, contents.data(), contents.size(), &bytes_read));
}

TEST_CASE("FAT16 VFS returns zero bytes at end of file")
{
    MakeFat16Disk("contents");
    REQUIRE(FeatherOS::VFS::MountBlockDevice());
    FeatherOS::VFS::FileHandle file{};
    REQUIRE(FeatherOS::VFS::Open("/BLOCK.TXT", &file));
    FeatherOS::VFS::FileInfo info{};
    REQUIRE(FeatherOS::VFS::Stat("/BLOCK.TXT", &info));
    REQUIRE(FeatherOS::VFS::Seek(&file, info.size));
    char byte;
    uint64_t bytes_read = 1;
    REQUIRE(FeatherOS::VFS::Read(&file, &byte, 1, &bytes_read));
    REQUIRE(bytes_read == 0);
}
namespace
{

std::string Pattern(size_t length)
{
    std::string text(length, '\0');
    for (size_t index = 0; index < length; ++index)
    {
        text[index] = static_cast<char>('a' + index % 23);
    }
    return text;
}

std::string ReadWhole(const char* path)
{
    FeatherOS::VFS::FileHandle file{};
    REQUIRE(FeatherOS::VFS::Open(path, &file));
    std::string contents(file.size, '\0');
    uint64_t bytes_read = 0;
    REQUIRE(FeatherOS::VFS::Read(&file, contents.data(), contents.size(), &bytes_read));
    REQUIRE(bytes_read == contents.size());
    FeatherOS::VFS::Close(&file);
    return contents;
}

uint64_t FreeDiskBlocks()
{
    FeatherDisk::VolumeStatistics statistics{};
    REQUIRE(FeatherOS::Filesystem::Drivers::Disk().Statistics(&statistics));
    return statistics.free_blocks;
}

uint16_t FatEntry(uint32_t cluster)
{
    const uint8_t* fat = disk[PartitionStart + 1].data();
    return static_cast<uint16_t>(fat[cluster * 2] | (fat[cluster * 2 + 1] << 8));
}

} // namespace

TEST_CASE("FAT16 creates a file and writes it across clusters")
{
    MakeFat16Disk("contents");
    REQUIRE(FeatherOS::VFS::MountBlockDevice());
    const uint64_t free_before = FreeDiskBlocks();

    FeatherOS::VFS::FileHandle file{};
    REQUIRE(FeatherOS::VFS::Create("/new.txt", &file));
    const std::string contents = Pattern(1500);
    for (size_t offset = 0; offset < contents.size(); offset += 333)
    {
        const size_t chunk = std::min<size_t>(333, contents.size() - offset);
        uint64_t written = 0;
        REQUIRE(FeatherOS::VFS::Write(&file, contents.data() + offset, chunk, &written));
        REQUIRE(written == chunk);
    }
    REQUIRE(file.size == contents.size());
    REQUIRE(FreeDiskBlocks() == free_before - 3);
    REQUIRE(std::memcmp(disk[RootDirectorySector].data() + 32, "NEW     TXT", 11) == 0);
    REQUIRE(ReadWhole("/NEW.TXT") == contents);
}

TEST_CASE("FAT16 overwrites in place and appends at a cluster boundary")
{
    MakeFat16Disk("contents");
    REQUIRE(FeatherOS::VFS::MountBlockDevice());
    FeatherOS::VFS::FileHandle file{};
    REQUIRE(FeatherOS::VFS::Create("/EDIT.BIN", &file));
    std::string expected = Pattern(512);
    uint64_t written = 0;
    REQUIRE(FeatherOS::VFS::Write(&file, expected.data(), expected.size(), &written));
    REQUIRE(FeatherOS::VFS::Write(&file, "tail", 4, &written));
    expected += "tail";
    REQUIRE(FeatherOS::VFS::Seek(&file, 100));
    REQUIRE(FeatherOS::VFS::Write(&file, "XYZ", 3, &written));
    expected.replace(100, 3, "XYZ");
    REQUIRE(file.size == expected.size());
    REQUIRE(ReadWhole("/EDIT.BIN") == expected);
}

TEST_CASE("FAT16 truncation frees the clusters a file no longer needs")
{
    MakeFat16Disk("contents");
    REQUIRE(FeatherOS::VFS::MountBlockDevice());
    FeatherOS::VFS::FileHandle file{};
    REQUIRE(FeatherOS::VFS::Create("/SHRINK.TXT", &file));
    const std::string contents = Pattern(2000);
    uint64_t written = 0;
    REQUIRE(FeatherOS::VFS::Write(&file, contents.data(), contents.size(), &written));
    const uint64_t free_full = FreeDiskBlocks();

    REQUIRE(FeatherOS::VFS::Truncate(&file, 600));
    REQUIRE(FreeDiskBlocks() == free_full + 2);
    REQUIRE(ReadWhole("/SHRINK.TXT") == contents.substr(0, 600));
    REQUIRE_FALSE(FeatherOS::VFS::Truncate(&file, 601));

    REQUIRE(FeatherOS::VFS::Truncate(&file, 0));
    REQUIRE(FreeDiskBlocks() == free_full + 4);
    // An empty file owns no cluster.
    REQUIRE(disk[RootDirectorySector][32 + 26] == 0);
    REQUIRE(disk[RootDirectorySector][32 + 27] == 0);
    REQUIRE(ReadWhole("/SHRINK.TXT").empty());
}

TEST_CASE("FAT16 removal frees the chain and the directory slot")
{
    MakeFat16Disk(std::string(700, 'x'));
    REQUIRE(FeatherOS::VFS::MountBlockDevice());
    const uint64_t free_before = FreeDiskBlocks();
    REQUIRE(FeatherOS::VFS::Remove("/BLOCK.TXT"));
    FeatherOS::VFS::FileInfo info{};
    REQUIRE_FALSE(FeatherOS::VFS::Stat("/BLOCK.TXT", &info));
    REQUIRE(FreeDiskBlocks() == free_before + 2);
    REQUIRE(FatEntry(2) == 0);
    REQUIRE(FatEntry(3) == 0);
    REQUIRE(disk[RootDirectorySector][0] == 0xE5);
    REQUIRE_FALSE(FeatherOS::VFS::Remove("/BLOCK.TXT"));

    FeatherOS::VFS::FileHandle file{};
    REQUIRE(FeatherOS::VFS::Create("/AGAIN.TXT", &file));
    REQUIRE(file.node.id == 0);
}

TEST_CASE("FAT16 rejects bad names, duplicates, read-only paths, and a full root directory")
{
    MakeFat16Disk("contents");
    REQUIRE(FeatherOS::VFS::MountBlockDevice());
    FeatherOS::VFS::FileHandle file{};
    REQUIRE_FALSE(FeatherOS::VFS::Create("/BLOCK.TXT", &file));
    REQUIRE_FALSE(FeatherOS::VFS::Create("/BAD*.TXT", &file));
    REQUIRE_FALSE(FeatherOS::VFS::Create(("/" + std::string(64, 'n')).c_str(), &file));
    REQUIRE_FALSE(FeatherOS::VFS::Create("/trailing.", &file));
    REQUIRE_FALSE(FeatherOS::VFS::Create("/a:b", &file));
    REQUIRE_FALSE(FeatherOS::VFS::Create("/system/hello.txt", &file));
    REQUIRE_FALSE(FeatherOS::VFS::Remove("/system/hello.txt"));
    REQUIRE_FALSE(FeatherOS::VFS::MakeDirectory("/system/sub"));

    // The fixture's root directory has 16 entries, and BLOCK.TXT uses one.
    for (int index = 0; index < 15; ++index)
    {
        const std::string path = "/F" + std::to_string(index) + ".TXT";
        REQUIRE(FeatherOS::VFS::Create(path.c_str(), &file));
    }
    REQUIRE_FALSE(FeatherOS::VFS::Create("/FULL.TXT", &file));
}

TEST_CASE("FAT16 stops a write cleanly when the disk is full")
{
    MakeFat16Disk("contents");
    REQUIRE(FeatherOS::VFS::MountBlockDevice());
    // Leave exactly one free cluster.
    const uint64_t free_clusters = FreeDiskBlocks();
    uint32_t marked = 0;
    for (uint32_t cluster = 3; marked + 1 < free_clusters; ++cluster)
    {
        uint8_t* entry = disk[PartitionStart + 1 + cluster / 256].data() + (cluster % 256) * 2;
        if (entry[0] == 0 && entry[1] == 0)
        {
            entry[0] = 0xFF;
            entry[1] = 0xFF;
            ++marked;
        }
    }
    REQUIRE(FreeDiskBlocks() == 1);

    FeatherOS::VFS::FileHandle file{};
    REQUIRE(FeatherOS::VFS::Create("/FULL.TXT", &file));
    const std::string contents = Pattern(1000);
    uint64_t written = 0;
    REQUIRE_FALSE(FeatherOS::VFS::Write(&file, contents.data(), contents.size(), &written));
    REQUIRE(written == 512);
    REQUIRE(file.size == 512);
    REQUIRE(ReadWhole("/FULL.TXT") == contents.substr(0, 512));
}

TEST_CASE("FAT16 refuses to free a looping cluster chain")
{
    MakeFat16Disk(std::string(700, 'x'));
    REQUIRE(FeatherOS::VFS::MountBlockDevice());
    WriteLittleEndian16(disk[PartitionStart + 1].data() + 6, 2);
    REQUIRE_FALSE(FeatherOS::VFS::Remove("/BLOCK.TXT"));
}

namespace
{

using FeatherOS::User::OpenAppend;
using FeatherOS::User::OpenCreate;
using FeatherOS::User::OpenRead;
using FeatherOS::User::OpenTruncate;
using FeatherOS::User::OpenWrite;

} // namespace

TEST_CASE("file descriptors start at 3, and the table grows to the soft limit")
{
    using namespace FeatherOS;
    MakeFat16Disk("contents");
    REQUIRE(VFS::MountBlockDevice());
    Files::Table table{};
    REQUIRE(Files::OpenConsole(&table, nullptr));
    REQUIRE(table.capacity == 32);
    std::vector<uint64_t> descriptors;
    for (uint64_t index = 0; index < User::DefaultDescriptorLimit - 3; ++index)
    {
        const uint64_t descriptor = Files::Open(&table, "/BLOCK.TXT", OpenRead);
        REQUIRE(descriptor == User::StandardError + 1 + index);
        descriptors.push_back(descriptor);
    }
    REQUIRE(table.capacity == User::DefaultDescriptorLimit);
    REQUIRE(Files::Open(&table, "/BLOCK.TXT", OpenRead) == User::SyscallErrorTooManyOpenFiles);
    REQUIRE(Files::CountOpen(&table) == User::DefaultDescriptorLimit);
    REQUIRE(Files::Close(&table, 100) == 0);
    REQUIRE(Files::Open(&table, "/BLOCK.TXT", OpenRead) == 100);
    Files::CloseAll(&table);
    REQUIRE(table.slots == nullptr);
    REQUIRE(table.capacity == 0);
    REQUIRE(Files::Close(&table, 0) == User::SyscallErrorBadDescriptor);
    REQUIRE(Files::Close(&table, descriptors[0]) == User::SyscallErrorBadDescriptor);
}

TEST_CASE("descriptor numbers are bounded by the soft limit, lookups by the capacity")
{
    using namespace FeatherOS;
    MakeFat16Disk("contents");
    REQUIRE(VFS::MountBlockDevice());
    Files::Table table{};
    REQUIRE(Files::OpenConsole(&table, nullptr));
    const uint64_t file = Files::Open(&table, "/BLOCK.TXT", OpenRead);
    REQUIRE(file == 3);
    REQUIRE(Files::Duplicate(&table, file, 200, 0) == 200);
    REQUIRE(table.capacity == 256);
    REQUIRE(Files::Duplicate(&table, file, 255, User::DuplicateExact) == 255);
    REQUIRE(Files::Duplicate(&table, file, 256, User::DuplicateExact) ==
            User::SyscallErrorBadDescriptor);
    REQUIRE(Files::Duplicate(&table, file, 256, 0) == User::SyscallErrorInvalidArgument);
    REQUIRE(Files::Duplicate(&table, file, 900, User::DuplicateExact) ==
            User::SyscallErrorBadDescriptor);
    REQUIRE(Files::Close(&table, 5000) == User::SyscallErrorBadDescriptor);
    REQUIRE(Files::KindOf(&table, 1ULL << 40) == Files::Kind::None);
    REQUIRE(Files::KindOf(&table, 255) == Files::Kind::File);
    REQUIRE(Files::KindOf(&table, 256) == Files::Kind::None);
    Files::CloseAll(&table);
}

TEST_CASE("an exact duplicate far above the capacity grows the table to the limit")
{
    using namespace FeatherOS;
    MakeFat16Disk("contents");
    REQUIRE(VFS::MountBlockDevice());
    Files::Table table{};
    REQUIRE(Files::OpenConsole(&table, nullptr));
    const User::ResourceLimits raised = {1024, 1024};
    REQUIRE(Files::DescriptorLimits(&table, &raised, nullptr) == 0);
    const uint64_t file = Files::Open(&table, "/BLOCK.TXT", OpenRead);
    REQUIRE(table.capacity == 32);
    REQUIRE(Files::Duplicate(&table, file, 900, User::DuplicateExact) == 900);
    REQUIRE(table.capacity == 1024);
    REQUIRE(Files::KindOf(&table, file) == Files::Kind::File);
    REQUIRE(Files::Duplicate(&table, file, 1023, 0) == 1023);
    REQUIRE(Files::Duplicate(&table, file, 1024, User::DuplicateExact) ==
            User::SyscallErrorBadDescriptor);
    Files::CloseAll(&table);
}

TEST_CASE("the descriptor limits are read, lowered, raised and checked")
{
    using namespace FeatherOS;
    MakeFat16Disk("contents");
    REQUIRE(VFS::MountBlockDevice());
    Files::Table table{};
    REQUIRE(Files::OpenConsole(&table, nullptr));
    User::ResourceLimits limits = {};
    REQUIRE(Files::DescriptorLimits(&table, nullptr, &limits) == 0);
    REQUIRE(limits.current == 256);
    REQUIRE(limits.maximum == 1024);

    for (uint64_t index = 3; index <= 50; ++index)
    {
        REQUIRE(Files::Open(&table, "/BLOCK.TXT", OpenRead) == index);
    }
    const User::ResourceLimits lowered = {40, 1024};
    REQUIRE(Files::DescriptorLimits(&table, &lowered, &limits) == 0);
    REQUIRE(limits.current == 256);
    REQUIRE(Files::Open(&table, "/BLOCK.TXT", OpenRead) == User::SyscallErrorTooManyOpenFiles);
    char byte[1];
    for (uint64_t index = 41; index <= 50; ++index)
    {
        REQUIRE(Files::Read(&table, index, byte, 1) == 1);
        REQUIRE(Files::Close(&table, index) == 0);
    }
    REQUIRE(Files::Open(&table, "/BLOCK.TXT", OpenRead) == User::SyscallErrorTooManyOpenFiles);
    REQUIRE(Files::Close(&table, 10) == 0);
    REQUIRE(Files::Open(&table, "/BLOCK.TXT", OpenRead) == 10);

    const User::ResourceLimits wider = {300, 1024};
    REQUIRE(Files::DescriptorLimits(&table, &wider, nullptr) == 0);
    uint64_t last = 0;
    for (;;)
    {
        const uint64_t descriptor = Files::Open(&table, "/BLOCK.TXT", OpenRead);
        if (User::IsSyscallError(descriptor))
        {
            REQUIRE(descriptor == User::SyscallErrorTooManyOpenFiles);
            break;
        }
        last = descriptor;
    }
    REQUIRE(last == 299);
    REQUIRE(table.capacity == 300);

    const User::ResourceLimits soft_above_hard = {500, 400};
    const User::ResourceLimits hard_above_ceiling = {10, 2000};
    const User::ResourceLimits infinite = {10, UINT64_MAX};
    REQUIRE(Files::DescriptorLimits(&table, &soft_above_hard, nullptr) ==
            User::SyscallErrorInvalidArgument);
    REQUIRE(Files::DescriptorLimits(&table, &hard_above_ceiling, nullptr) ==
            User::SyscallErrorNotPermitted);
    REQUIRE(Files::DescriptorLimits(&table, &infinite, nullptr) == User::SyscallErrorNotPermitted);
    REQUIRE(Files::DescriptorLimits(&table, nullptr, &limits) == 0);
    REQUIRE(limits.current == 300);
    REQUIRE(limits.maximum == 1024);
    const User::ResourceLimits low_hard = {10, 100};
    const User::ResourceLimits high_again = {10, 1024};
    REQUIRE(Files::DescriptorLimits(&table, &low_hard, nullptr) == 0);
    REQUIRE(Files::DescriptorLimits(&table, &high_again, nullptr) == 0);
    REQUIRE(Files::DescriptorLimits(&table, nullptr, &limits) == 0);
    REQUIRE(limits.current == 10);
    REQUIRE(limits.maximum == 1024);
    Files::CloseAll(&table);
}

TEST_CASE("a child inherits the open descriptors that are not close-on-exec, and the limits")
{
    using namespace FeatherOS;
    MakeFat16Disk("contents");
    REQUIRE(VFS::MountBlockDevice());
    Files::Table parent{};
    REQUIRE(Files::OpenConsole(&parent, nullptr));
    const User::ResourceLimits limits = {300, 1024};
    REQUIRE(Files::DescriptorLimits(&parent, &limits, nullptr) == 0);
    REQUIRE(Files::Open(&parent, "/BLOCK.TXT", OpenRead | User::OpenCloseOnExec) == 3);
    const uint64_t shared = Files::Open(&parent, "/BLOCK.TXT", OpenRead);
    REQUIRE(Files::Duplicate(&parent, shared, 200, 0) == 200);
    REQUIRE(parent.capacity == 256);

    Files::Table child{};
    REQUIRE(Files::Inherit(&child, &parent));
    REQUIRE(child.capacity == 256);
    REQUIRE(child.soft_limit == 300);
    REQUIRE(child.hard_limit == 1024);
    REQUIRE(Files::KindOf(&child, 0) == Files::Kind::Terminal);
    REQUIRE(Files::KindOf(&child, 2) == Files::Kind::Terminal);
    REQUIRE(Files::KindOf(&child, 3) == Files::Kind::None);
    REQUIRE(Files::KindOf(&child, 200) == Files::Kind::File);
    REQUIRE(Files::CountOpen(&child) == 5);
    char text[3] = {};
    REQUIRE(Files::Read(&child, 200, text, 3) == 3);
    REQUIRE(Files::Read(&parent, 200, text, 3) == 3);
    REQUIRE(std::string(text, 3) == "ten");
    Files::CloseAll(&parent);
    Files::CloseAll(&child);
    REQUIRE(Files::Remove("/BLOCK.TXT") == 0);
}

TEST_CASE("a pipe's two ends can straddle a growth of the table")
{
    using namespace FeatherOS;
    MakeFat16Disk("contents");
    REQUIRE(VFS::MountBlockDevice());
    Files::Table table{};
    REQUIRE(Files::OpenConsole(&table, nullptr));
    for (uint64_t index = 3; index < 31; ++index)
    {
        REQUIRE(Files::Open(&table, "/BLOCK.TXT", OpenRead) == index);
    }
    uint64_t read_end = 0;
    uint64_t write_end = 0;
    REQUIRE(Files::CreatePipe(&table, 0, &read_end, &write_end) == 0);
    REQUIRE(read_end == 31);
    REQUIRE(write_end == 32);
    REQUIRE(table.capacity == 64);
    REQUIRE(Files::Write(&table, write_end, "ok", 2) == 2);
    char text[2] = {};
    REQUIRE(Files::Read(&table, read_end, text, 2) == 2);
    REQUIRE(std::string(text, 2) == "ok");
    REQUIRE(Files::KindOf(&table, 3) == Files::Kind::File);
    Files::CloseAll(&table);
}

TEST_CASE("a table that cannot grow fails with no memory and keeps what it has")
{
    using namespace FeatherOS;
    MakeFat16Disk("contents");
    REQUIRE(VFS::MountBlockDevice());
    Files::Table table{};
    REQUIRE(Files::OpenConsole(&table, nullptr));
    for (uint64_t index = 3; index < 32; ++index)
    {
        REQUIRE(Files::Open(&table, "/BLOCK.TXT", OpenRead) == index);
    }
    {
        const Memory::Testing::FailZeroedAllocations fail(64, sizeof(Files::Slot));
        REQUIRE(Files::Open(&table, "/BLOCK.TXT", OpenRead) == User::SyscallErrorNoMemory);
        REQUIRE(Files::Duplicate(&table, 3, 40, User::DuplicateExact) ==
                User::SyscallErrorNoMemory);
        uint64_t read_end = 0;
        uint64_t write_end = 0;
        REQUIRE(Files::CreatePipe(&table, 0, &read_end, &write_end) == User::SyscallErrorNoMemory);
        REQUIRE(table.capacity == 32);
    }
    REQUIRE(Files::CountOpen(&table) == 32);
    REQUIRE(Files::Open(&table, "/BLOCK.TXT", OpenRead) == 32);
    REQUIRE(table.capacity == 64);

    Files::Table child{};
    {
        const Memory::Testing::FailZeroedAllocations fail(64, sizeof(Files::Slot));
        REQUIRE_FALSE(Files::Inherit(&child, &table));
        REQUIRE(child.slots == nullptr);
        REQUIRE(child.capacity == 0);
    }
    REQUIRE(Files::Inherit(&child, &table));
    Files::CloseAll(&child);
    Files::CloseAll(&table);
    REQUIRE(Files::Remove("/BLOCK.TXT") == 0);
}

TEST_CASE("file descriptors validate flags and access modes")
{
    MakeFat16Disk("contents");
    REQUIRE(FeatherOS::VFS::MountBlockDevice());
    const std::vector<uint8_t> archive = MakeTarArchive("system/hello.txt", "archive");
    REQUIRE(FeatherOS::Initramfs::Mount(archive.data(), archive.size()));
    FeatherOS::Files::Table table{};
    FeatherOS::Files::OpenConsole(&table, nullptr);
    REQUIRE(FeatherOS::Files::Open(&table, "/BLOCK.TXT", 0) ==
            FeatherOS::User::SyscallErrorInvalidArgument);
    REQUIRE(FeatherOS::Files::Open(&table, "/BLOCK.TXT", OpenRead | 512) ==
            FeatherOS::User::SyscallErrorInvalidArgument);
    REQUIRE(FeatherOS::Files::Open(&table, "/NEW.TXT", OpenRead | OpenCreate) ==
            FeatherOS::User::SyscallErrorInvalidArgument);
    REQUIRE(FeatherOS::Files::Open(&table, "/NEW.TXT", OpenWrite) ==
            FeatherOS::User::SyscallErrorNotFound);
    REQUIRE(FeatherOS::Files::Open(&table, "hello.txt", OpenWrite) ==
            FeatherOS::User::SyscallErrorReadOnly);
    REQUIRE(FeatherOS::Files::Remove("hello.txt") == FeatherOS::User::SyscallErrorReadOnly);

    const uint64_t reader = FeatherOS::Files::Open(&table, "hello.txt", OpenRead);
    char buffer[16];
    REQUIRE(FeatherOS::Files::Read(&table, reader, buffer, sizeof(buffer)) == 7);
    REQUIRE(FeatherOS::Files::Write(&table, reader, buffer, 1) ==
            FeatherOS::User::SyscallErrorBadDescriptor);
    const uint64_t writer = FeatherOS::Files::Open(&table, "/NEW.TXT", OpenWrite | OpenCreate);
    REQUIRE(FeatherOS::Files::Read(&table, writer, buffer, 1) ==
            FeatherOS::User::SyscallErrorBadDescriptor);
    FeatherOS::Files::CloseAll(&table);
}

TEST_CASE("file descriptors read, write, seek, and truncate disk files")
{
    MakeFat16Disk("contents");
    REQUIRE(FeatherOS::VFS::MountBlockDevice());
    FeatherOS::Files::Table table{};
    FeatherOS::Files::OpenConsole(&table, nullptr);
    const uint64_t writer =
        FeatherOS::Files::Open(&table, "/DATA.TXT", OpenWrite | OpenCreate | OpenTruncate);
    REQUIRE_FALSE(FeatherOS::User::IsSyscallError(writer));
    const std::string contents = Pattern(1200);
    REQUIRE(FeatherOS::Files::Write(&table, writer, contents.data(), contents.size()) ==
            contents.size());
    REQUIRE(FeatherOS::Files::Seek(&table, writer, 1201, FeatherOS::User::SeekFromStart) ==
            FeatherOS::User::SyscallErrorInvalidArgument);
    REQUIRE(FeatherOS::Files::Seek(&table, writer, 1200, FeatherOS::User::SeekFromStart) == 1200);
    REQUIRE(FeatherOS::Files::Write(&table, writer, "END", 3) == 3);
    REQUIRE(FeatherOS::Files::Close(&table, writer) == 0);

    const uint64_t reader = FeatherOS::Files::Open(&table, "/DATA.TXT", OpenRead);
    REQUIRE(FeatherOS::Files::Seek(&table, reader, 1190, FeatherOS::User::SeekFromStart) == 1190);
    REQUIRE(FeatherOS::Files::Seek(&table, reader, static_cast<uint64_t>(-10),
                                   FeatherOS::User::SeekFromCurrent) == 1180);
    REQUIRE(FeatherOS::Files::Seek(&table, reader, static_cast<uint64_t>(-3),
                                   FeatherOS::User::SeekFromEnd) == 1200);
    REQUIRE(FeatherOS::Files::Seek(&table, reader, static_cast<uint64_t>(-1204),
                                   FeatherOS::User::SeekFromEnd) ==
            FeatherOS::User::SyscallErrorInvalidArgument);
    REQUIRE(FeatherOS::Files::Seek(&table, reader, 0, 3) ==
            FeatherOS::User::SyscallErrorInvalidArgument);
    REQUIRE(FeatherOS::Files::Seek(&table, reader, 1190, FeatherOS::User::SeekFromStart) == 1190);
    char tail[32];
    REQUIRE(FeatherOS::Files::Read(&table, reader, tail, sizeof(tail)) == 13);
    REQUIRE(std::string(tail, 13) == contents.substr(1190) + "END");
    REQUIRE(FeatherOS::Files::Read(&table, reader, tail, sizeof(tail)) == 0);
    REQUIRE(FeatherOS::Files::Close(&table, reader) == 0);

    const uint64_t truncating =
        FeatherOS::Files::Open(&table, "/DATA.TXT", OpenWrite | OpenTruncate);
    REQUIRE(FeatherOS::Files::Close(&table, truncating) == 0);
    REQUIRE(ReadWhole("/DATA.TXT").empty());
}

TEST_CASE("disk files allow one writer or many readers, and open files cannot be removed")
{
    MakeFat16Disk("contents");
    REQUIRE(FeatherOS::VFS::MountBlockDevice());
    FeatherOS::Files::Table first{};
    FeatherOS::Files::OpenConsole(&first, nullptr);
    FeatherOS::Files::Table second{};
    FeatherOS::Files::OpenConsole(&second, nullptr);
    const uint64_t reader = FeatherOS::Files::Open(&first, "/BLOCK.TXT", OpenRead);
    REQUIRE_FALSE(FeatherOS::User::IsSyscallError(
        FeatherOS::Files::Open(&second, "/BLOCK.TXT", OpenRead)));
    REQUIRE(FeatherOS::Files::Open(&second, "/BLOCK.TXT", OpenWrite) ==
            FeatherOS::User::SyscallErrorBusy);
    REQUIRE(FeatherOS::Files::Remove("/BLOCK.TXT") == FeatherOS::User::SyscallErrorBusy);
    FeatherOS::Files::CloseAll(&second);
    REQUIRE(FeatherOS::Files::Close(&first, reader) == 0);

    const uint64_t writer = FeatherOS::Files::Open(&first, "/BLOCK.TXT", OpenWrite);
    REQUIRE_FALSE(FeatherOS::User::IsSyscallError(writer));
    REQUIRE(FeatherOS::Files::Open(&second, "/BLOCK.TXT", OpenRead) ==
            FeatherOS::User::SyscallErrorBusy);
    // Exiting processes close everything, which makes the file removable again.
    FeatherOS::Files::CloseAll(&first);
    REQUIRE(FeatherOS::Files::Remove("/BLOCK.TXT") == 0);
    REQUIRE(FeatherOS::Files::Remove("/BLOCK.TXT") == FeatherOS::User::SyscallErrorNotFound);
}

TEST_CASE("a full system-wide disk table gives ENFILE, and only for a file not open yet")
{
    using namespace FeatherOS;
    MakeManyFilesDisk();
    REQUIRE(VFS::MountBlockDevice());
    Files::Table first{};
    Files::Table second{};
    REQUIRE(Files::OpenConsole(&first, nullptr));
    REQUIRE(Files::OpenConsole(&second, nullptr));
    const User::ResourceLimits limits = {1024, 1024};
    REQUIRE(Files::DescriptorLimits(&first, &limits, nullptr) == 0);
    REQUIRE(Files::DescriptorLimits(&second, &limits, nullptr) == 0);

    std::vector<uint64_t> first_files;
    for (uint32_t number = 0; number < 600; ++number)
    {
        first_files.push_back(Files::Open(&first, ManyFileName(number).c_str(), OpenRead));
        REQUIRE_FALSE(User::IsSyscallError(first_files.back()));
    }
    for (uint32_t number = 600; number < 1024; ++number)
    {
        REQUIRE_FALSE(User::IsSyscallError(Files::Open(&second, ManyFileName(number).c_str(), OpenRead)));
    }
    const std::string last = ManyFileName(1024);
    REQUIRE(Files::Open(&second, last.c_str(), OpenRead) == User::SyscallErrorTooManyFilesInSystem);

    REQUIRE_FALSE(User::IsSyscallError(Files::Open(&second, ManyFileName(0).c_str(), OpenRead)));

    User::FileStatus status;
    REQUIRE(Files::Open(&second, "/NEWFILE.TXT", OpenWrite | OpenCreate) ==
            User::SyscallErrorTooManyFilesInSystem);
    REQUIRE(Files::Stat("/NEWFILE.TXT", &status) == User::SyscallErrorNotFound);

    REQUIRE(Files::Close(&first, first_files[5]) == 0);
    REQUIRE_FALSE(User::IsSyscallError(Files::Open(&second, last.c_str(), OpenRead)));
    Files::CloseAll(&first);
    Files::CloseAll(&second);
}

TEST_CASE("the disk-file table grows and keeps every entry and every open file's offset")
{
    using namespace FeatherOS;
    REQUIRE(Files::ResetDiskFileTable());
    MakeManyFilesDisk();
    REQUIRE(VFS::MountBlockDevice());
    Files::Table table{};
    REQUIRE(Files::OpenConsole(&table, nullptr));
    const User::ResourceLimits limits = {1024, 1024};
    REQUIRE(Files::DescriptorLimits(&table, &limits, nullptr) == 0);

    std::vector<uint64_t> descriptors;
    const uint64_t writer = Files::Open(&table, ManyFileName(0).c_str(), OpenWrite);
    REQUIRE_FALSE(User::IsSyscallError(writer));
    descriptors.push_back(writer);
    const auto check_all_busy = [&descriptors]
    {
        for (uint32_t number = 0; number < descriptors.size(); ++number)
        {
            REQUIRE(Files::Remove(ManyFileName(number).c_str()) == User::SyscallErrorBusy);
        }
        REQUIRE(Files::Remount("/", Filesystem::MountMode::ReadOnly) == User::SyscallErrorBusy);
    };
    const auto open_up_to = [&](uint32_t count)
    {
        while (descriptors.size() < count)
        {
            const auto number = static_cast<uint32_t>(descriptors.size());
            const uint64_t descriptor = Files::Open(&table, ManyFileName(number).c_str(), OpenRead);
            REQUIRE_FALSE(User::IsSyscallError(descriptor));
            char first[1];
            REQUIRE(Files::Read(&table, descriptor, first, 1) == 1);
            REQUIRE(first[0] == 'A' + static_cast<char>(number % 26));
            descriptors.push_back(descriptor);
        }
    };
    open_up_to(64);
    check_all_busy();
    open_up_to(65);
    check_all_busy();
    open_up_to(129);
    check_all_busy();
    open_up_to(300);
    check_all_busy();
    for (uint32_t number = 1; number < descriptors.size(); ++number)
    {
        char second[1];
        REQUIRE(Files::Read(&table, descriptors[number], second, 1) == 1);
        REQUIRE(second[0] == 'a' + static_cast<char>(number % 26));
    }

    Files::CloseAll(&table);
    REQUIRE(Files::Remount("/", Filesystem::MountMode::ReadOnly) == 0);
}

TEST_CASE("a disk table that cannot grow gives no memory and creates nothing")
{
    using namespace FeatherOS;
    REQUIRE(Files::ResetDiskFileTable());
    MakeManyFilesDisk();
    REQUIRE(VFS::MountBlockDevice());
    Files::Table table{};
    REQUIRE(Files::OpenConsole(&table, nullptr));
    for (uint32_t number = 0; number < 64; ++number)
    {
        REQUIRE_FALSE(User::IsSyscallError(Files::Open(&table, ManyFileName(number).c_str(), OpenRead)));
    }
    {
        const Memory::Testing::FailZeroedAllocations fail(128, 24);
        User::FileStatus status;
        REQUIRE(Files::Open(&table, ManyFileName(64).c_str(), OpenRead) ==
                User::SyscallErrorNoMemory);
        REQUIRE(Files::Open(&table, "/NEWFILE.TXT", OpenWrite | OpenCreate) ==
                User::SyscallErrorNoMemory);
        REQUIRE(Files::Stat("/NEWFILE.TXT", &status) == User::SyscallErrorNotFound);
        REQUIRE(Files::CountOpen(&table) == 3 + 64);
        REQUIRE_FALSE(User::IsSyscallError(Files::Open(&table, ManyFileName(3).c_str(), OpenRead)));
    }
    REQUIRE_FALSE(User::IsSyscallError(Files::Open(&table, ManyFileName(64).c_str(), OpenRead)));
    Files::CloseAll(&table);
}

TEST_CASE("every open disk file counts, however many are open, for a remount's busy check")
{
    using namespace FeatherOS;
    MakeManyFilesDisk();
    REQUIRE(VFS::MountBlockDevice());
    Files::Table table{};
    REQUIRE(Files::OpenConsole(&table, nullptr));
    for (uint32_t number = 0; number < 70; ++number)
    {
        REQUIRE_FALSE(User::IsSyscallError(Files::Open(&table, ManyFileName(number).c_str(), OpenRead)));
    }
    const uint64_t writer = Files::Open(&table, "/tmp/held.txt", OpenWrite | OpenCreate);
    REQUIRE_FALSE(User::IsSyscallError(writer));
    REQUIRE(Files::Remount("/tmp", Filesystem::MountMode::ReadOnly) == User::SyscallErrorBusy);
    REQUIRE(Files::Close(&table, writer) == 0);
    REQUIRE(Files::Remount("/tmp", Filesystem::MountMode::ReadOnly) == 0);
    REQUIRE(Files::Remount("/tmp", Filesystem::MountMode::ReadWrite) == 0);
    REQUIRE(Files::Remove("/tmp/held.txt") == 0);
    Files::CloseAll(&table);
}

TEST_CASE("directory listing skips deleted entries and names files as NAME.EXT")
{
    MakeFat16Disk("contents");
    REQUIRE(FeatherOS::VFS::MountBlockDevice());
    FeatherOS::VFS::FileHandle file{};
    REQUIRE(FeatherOS::VFS::Create("/GONE.TXT", &file));
    REQUIRE(FeatherOS::VFS::Create("/README", &file));
    REQUIRE(FeatherOS::VFS::Remove("/GONE.TXT"));

    FeatherOS::Files::Table table{};
    FeatherOS::Files::OpenConsole(&table, nullptr);
    const uint64_t writer =
        FeatherOS::Files::Open(&table, "/README", OpenWrite | OpenAppend);
    REQUIRE(FeatherOS::Files::Write(&table, writer, "abc", 3) == 3);
    REQUIRE(FeatherOS::Files::Close(&table, writer) == 0);

    FeatherOS::User::DirectoryEntry entry{};
    // Indices 0-4 of the root are the mount points.
    REQUIRE(FeatherOS::Files::ReadDirectory("/", 0, &entry) == 0);
    REQUIRE(std::string(entry.name) == "system");
    REQUIRE(entry.type == FeatherOS::User::ModeDirectory >> FeatherOS::User::EntryTypeShift);
    REQUIRE(FeatherOS::Files::ReadDirectory("/", 5, &entry) == 0);
    REQUIRE(std::string(entry.name) == "BLOCK.TXT");
    REQUIRE(entry.size == 8);
    REQUIRE(FeatherOS::Files::ReadDirectory("/", 6, &entry) == 0);
    REQUIRE(std::string(entry.name) == "README");
    REQUIRE(entry.size == 3);
    REQUIRE(FeatherOS::Files::ReadDirectory("/", 7, &entry) ==
            FeatherOS::User::SyscallErrorNotFound);
    REQUIRE(FeatherOS::Files::ReadDirectory("/missing", 0, &entry) ==
            FeatherOS::User::SyscallErrorNotFound);
    REQUIRE(FeatherOS::Files::ReadDirectory("/README", 0, &entry) ==
            FeatherOS::User::SyscallErrorNotDirectory);
}

TEST_CASE("appending opens at the end of the file")
{
    MakeFat16Disk("contents");
    REQUIRE(FeatherOS::VFS::MountBlockDevice());
    FeatherOS::Files::Table table{};
    FeatherOS::Files::OpenConsole(&table, nullptr);
    REQUIRE(FeatherOS::Files::Open(&table, "/BLOCK.TXT", OpenRead | OpenAppend) ==
            FeatherOS::User::SyscallErrorInvalidArgument);
    const uint64_t appender =
        FeatherOS::Files::Open(&table, "/BLOCK.TXT", OpenWrite | OpenAppend);
    REQUIRE(FeatherOS::Files::Write(&table, appender, "+more", 5) == 5);
    REQUIRE(FeatherOS::Files::Close(&table, appender) == 0);
    REQUIRE(ReadWhole("/BLOCK.TXT") == "contents+more");
}

namespace
{

constexpr uint32_t GptPartitionStart = 64;
constexpr uint32_t GptEntryCount = 128;
constexpr uint32_t GptEntrySize = 128;

uint32_t Crc32(const uint8_t* bytes, size_t length)
{
    uint32_t crc = 0xFFFFFFFFU;
    for (size_t index = 0; index < length; ++index)
    {
        crc ^= bytes[index];
        for (int bit = 0; bit < 8; ++bit)
        {
            crc = (crc >> 1) ^ ((crc & 1U) != 0 ? 0xEDB88320U : 0U);
        }
    }
    return ~crc;
}

void WriteLittleEndian64(uint8_t* destination, uint64_t value)
{
    WriteLittleEndian32(destination, static_cast<uint32_t>(value));
    WriteLittleEndian32(destination + 4, static_cast<uint32_t>(value >> 32));
}

// Moves the fixture's FAT16 partition behind a protective MBR and a GUID partition table.
void ConvertToGpt()
{
    const std::vector<std::array<uint8_t, FeatherOS::BlockDevice::SectorSize>> partition(
        disk.begin() + PartitionStart, disk.end());
    disk.assign(GptPartitionStart + partition.size() + 33, {});
    std::copy(partition.begin(), partition.end(), disk.begin() + GptPartitionStart);

    uint8_t* mbr = disk[0].data();
    mbr[446 + 4] = 0xEE;
    WriteLittleEndian32(mbr + 446 + 8, 1);
    WriteLittleEndian32(mbr + 446 + 12, 0xFFFFFFFFU);
    mbr[510] = 0x55;
    mbr[511] = 0xAA;

    std::vector<uint8_t> entries(GptEntryCount * GptEntrySize, 0);
    entries[0] = 0xA2; // Any non-zero type GUID marks the entry as used.
    WriteLittleEndian64(entries.data() + 32, GptPartitionStart);
    WriteLittleEndian64(entries.data() + 40, GptPartitionStart + partition.size() - 1);
    for (size_t sector = 0; sector < entries.size() / FeatherOS::BlockDevice::SectorSize; ++sector)
    {
        std::memcpy(disk[2 + sector].data(),
                    entries.data() + sector * FeatherOS::BlockDevice::SectorSize,
                    FeatherOS::BlockDevice::SectorSize);
    }

    uint8_t* header = disk[1].data();
    std::memcpy(header, "EFI PART", 8);
    WriteLittleEndian32(header + 8, 0x00010000);
    WriteLittleEndian32(header + 12, 92);
    WriteLittleEndian64(header + 24, 1);
    WriteLittleEndian64(header + 32, disk.size() - 1);
    WriteLittleEndian64(header + 40, 34);
    WriteLittleEndian64(header + 48, disk.size() - 34);
    WriteLittleEndian64(header + 72, 2);
    WriteLittleEndian32(header + 80, GptEntryCount);
    WriteLittleEndian32(header + 84, GptEntrySize);
    WriteLittleEndian32(header + 88, Crc32(entries.data(), entries.size()));
    WriteLittleEndian32(header + 16, Crc32(header, 92));
}

// "<source> <type> <ro|rw>" of the mount at point, as /proc/mounts lists it.
std::string MountSource(const std::string& point)
{
    FeatherOS::Procfs::MountStatus status{};
    for (uint32_t index = 0; FeatherOS::VFS::DescribeMount(index, &status); ++index)
    {
        if (point == status.point)
        {
            return std::string(status.source) + " " + status.type +
                   (status.read_only ? " ro" : " rw");
        }
    }
    return "";
}

} // namespace

TEST_CASE("FAT16 mounts a partition from a GUID partition table")
{
    MakeFat16Disk("gpt contents");
    ConvertToGpt();
    REQUIRE(FeatherOS::VFS::MountBlockDevice());
    REQUIRE(ReadWhole("/BLOCK.TXT") == "gpt contents");
    FeatherOS::VFS::FileHandle file{};
    REQUIRE(FeatherOS::VFS::Create("/NEW.TXT", &file));
    uint64_t written = 0;
    REQUIRE(FeatherOS::VFS::Write(&file, "x", 1, &written));
    REQUIRE(ReadWhole("/NEW.TXT") == "x");
}

TEST_CASE("the disk's FAT16 mount skips a Linux partition listed before it")
{
    using FeatherOS::VFS::MountResult;
    MakeFat16Disk("fat contents");
    constexpr uint32_t LinuxStart = PartitionStart + PartitionSectorCount;
    constexpr uint32_t LinuxSectors = 64;
    disk.resize(LinuxStart + LinuxSectors, {});
    // An ext2 magic number where ext2 keeps it, which FAT16's probe must not care about.
    WriteLittleEndian16(disk[LinuxStart + 2].data() + 56, 0xEF53);
    uint8_t* table = disk[0].data() + 446;
    std::memcpy(table + 16, table, 16);
    std::memset(table, 0, 16);
    table[4] = 0x83;
    WriteLittleEndian32(table + 8, LinuxStart);
    WriteLittleEndian32(table + 12, LinuxSectors);
    REQUIRE(FeatherOS::VFS::MountBlockDevice());
    REQUIRE(ReadWhole("/BLOCK.TXT") == "fat contents");
    REQUIRE(FeatherOS::Filesystem::Drivers::Disk().PartitionNumber() == 2);
    REQUIRE(MountSource("/") == "vda2 fat16 rw");
    REQUIRE(FeatherOS::VFS::Mount("ext2", "vda1", "/mnt") == MountResult::UnknownType);
}

TEST_CASE("a FAT16 volume in an extended partition is vda5")
{
    MakeFat16Disk("logical contents");
    disk.insert(disk.begin() + 1, 2, {});
    uint8_t* table = disk[0].data() + 446;
    table[4] = 0x05;
    WriteLittleEndian32(table + 8, 1);
    WriteLittleEndian32(table + 12, 5002);
    uint8_t* ebr = disk[1].data();
    ebr[446 + 4] = 0x06;
    WriteLittleEndian32(ebr + 446 + 8, 2);
    WriteLittleEndian32(ebr + 446 + 12, PartitionSectorCount);
    ebr[510] = 0x55;
    ebr[511] = 0xAA;
    REQUIRE(FeatherOS::VFS::MountBlockDevice());
    REQUIRE(ReadWhole("/BLOCK.TXT") == "logical contents");
    REQUIRE(FeatherOS::Filesystem::Drivers::Disk().PartitionNumber() == 5);
    REQUIRE(MountSource("/") == "vda5 fat16 rw");
}

TEST_CASE("disk sources are named as Linux names the first virtio disk's partitions")
{
    using FeatherOS::Filesystem::Drivers::PartitionSource;
    REQUIRE(std::string(PartitionSource(0)) == "vda");
    REQUIRE(std::string(PartitionSource(1)) == "vda1");
    REQUIRE(std::string(PartitionSource(2)) == "vda2");
    REQUIRE(std::string(PartitionSource(10)) == "vda10");
    REQUIRE(std::string(PartitionSource(16)) == "vda16");
    REQUIRE(PartitionSource(17) == nullptr);

    MakeFat16Disk("contents");
    REQUIRE(FeatherOS::VFS::MountBlockDevice());
    REQUIRE(FeatherOS::Filesystem::Drivers::Disk().PartitionNumber() == 1);
    REQUIRE(MountSource("/") == "vda1 fat16 rw");
}


namespace
{

std::vector<std::string> ListDirectory(const char* path)
{
    std::vector<std::string> names;
    FeatherOS::Filesystem::DirectoryEntry entry{};
    for (uint32_t index = 0; FeatherOS::VFS::ReadDirectory(path, index, &entry); ++index)
    {
        names.push_back(std::string(entry.name) +
                        (entry.type == FeatherOS::User::ModeDirectory ? "/" : ""));
    }
    return names;
}

// The disk's root entries, without the mount points listed first.
std::vector<std::string> ListDiskRoot()
{
    std::vector<std::string> names = ListDirectory("/");
    const std::vector<std::string> mounts = {"system/", "etc/", "dev/", "proc/", "tmp/"};
    REQUIRE(names.size() >= mounts.size());
    REQUIRE(std::vector<std::string>(names.begin(), names.begin() + mounts.size()) == mounts);
    names.erase(names.begin(), names.begin() + mounts.size());
    return names;
}

std::vector<std::string> Sorted(std::vector<std::string> names)
{
    std::sort(names.begin(), names.end());
    return names;
}

void WriteWhole(const char* path, const std::string& contents)
{
    FeatherOS::VFS::FileHandle file{};
    REQUIRE(FeatherOS::VFS::Create(path, &file));
    uint64_t written = 0;
    REQUIRE(FeatherOS::VFS::Write(&file, contents.data(), contents.size(), &written));
    REQUIRE(written == contents.size());
}

void WriteAndClose(const char* path, const std::string& contents)
{
    FeatherOS::VFS::FileHandle file{};
    REQUIRE(FeatherOS::VFS::Create(path, &file));
    uint64_t written = 0;
    REQUIRE(FeatherOS::VFS::Write(&file, contents.data(), contents.size(), &written));
    REQUIRE(written == contents.size());
    FeatherOS::VFS::Close(&file);
}

} // namespace

namespace
{

struct WriteFailureGuard
{
    ~WriteFailureGuard()
    {
        writes_fail = false;
        on_write = nullptr;
    }
};

// What rename onto an existing name does under base, on a filesystem that is mounted and writable.
void CheckReplacementRules(const std::string& base, bool links, bool hard_links)
{
    namespace Files = FeatherOS::Files;
    namespace User = FeatherOS::User;
    const auto path = [&](const char* name) { return base + "/" + name; };
    const auto rename = [&](const char* from, const char* to) {
        return Files::Rename(path(from).c_str(), path(to).c_str());
    };
    const auto put = [&](const char* name, const std::string& bytes) {
        WriteAndClose(path(name).c_str(), bytes);
    };
    const auto kind = [&](const char* name) {
        User::FileStatus status{};
        REQUIRE(Files::LinkStat(path(name).c_str(), &status) == 0);
        return status.mode & User::ModeTypeMask;
    };
    const auto missing = [&](const char* name) {
        User::FileStatus status{};
        return Files::LinkStat(path(name).c_str(), &status) == User::SyscallErrorNotFound;
    };
    const auto make_directory = [&](const char* name) {
        REQUIRE(Files::MakeDirectory(path(name).c_str()) == 0);
    };
    REQUIRE(Files::MakeDirectory(base.c_str()) == 0);
    make_directory("sub");

    put("a", "A");
    put("b", "B");
    REQUIRE(rename("a", "b") == 0);
    REQUIRE(ReadWhole(path("b").c_str()) == "A");
    REQUIRE(missing("a"));
    put("c", "C");
    put("sub/b", "old");
    REQUIRE(rename("c", "sub/b") == 0);
    REQUIRE(ReadWhole(path("sub/b").c_str()) == "C");
    REQUIRE(missing("c"));
    REQUIRE(rename("b", "b") == 0);

    if (links)
    {
        REQUIRE(Files::SymbolicLink("b", path("ln").c_str()) == 0);
        put("f", "F");
        REQUIRE(rename("f", "ln") == 0);
        REQUIRE(kind("ln") == User::ModeRegular);
        REQUIRE(ReadWhole(path("ln").c_str()) == "F");
        REQUIRE(Files::SymbolicLink("nowhere", path("ln2").c_str()) == 0);
        put("g", "G");
        REQUIRE(rename("ln2", "g") == 0);
        REQUIRE(kind("g") == User::ModeSymbolicLink);
        REQUIRE(Files::SymbolicLink("nowhere", path("ln3").c_str()) == 0);
        REQUIRE(Files::SymbolicLink("somewhere", path("ln4").c_str()) == 0);
        REQUIRE(rename("ln3", "ln4") == 0);
        REQUIRE(kind("ln4") == User::ModeSymbolicLink);
        make_directory("by-link");
        REQUIRE(rename("by-link", "ln4") == User::SyscallErrorNotDirectory);
    }

    make_directory("empty");
    make_directory("full");
    make_directory("full/in");
    put("full/child", "child");
    put("x", "X");
    uint64_t writes = 0;
    on_write = [&](uint64_t /*sector*/, const void* /*data*/) { ++writes; };
    const WriteFailureGuard guard;
    REQUIRE(rename("x", "empty") == User::SyscallErrorIsDirectory);
    REQUIRE(rename("empty", "x") == User::SyscallErrorNotDirectory);
    REQUIRE(rename("empty", "full") == User::SyscallErrorNotEmpty);
    REQUIRE(rename("full/in", "full") == User::SyscallErrorNotEmpty);
    REQUIRE(rename("full", "full/in/deeper") == User::SyscallErrorInvalidArgument);
    REQUIRE(rename("x", "nodir/x") == User::SyscallErrorNotFound);
    REQUIRE(rename("missing", "x") == User::SyscallErrorNotFound);
    REQUIRE(writes == 0);
    REQUIRE(ReadWhole(path("x").c_str()) == "X");
    REQUIRE(ReadWhole(path("full/child").c_str()) == "child");

    if (hard_links)
    {
        REQUIRE(Files::Link(path("x").c_str(), path("x2").c_str()) == 0);
        writes = 0;
        REQUIRE(rename("x", "x2") == 0);
        REQUIRE(rename("x2", "x") == 0);
        REQUIRE(writes == 0);
        User::FileStatus status{};
        REQUIRE(Files::Stat(path("x").c_str(), &status) == 0);
        REQUIRE(status.link_count == 2);
        REQUIRE(Files::Stat(path("x2").c_str(), &status) == 0);
        REQUIRE(Files::Remove(path("x2").c_str()) == 0);
    }
    on_write = nullptr;

    make_directory("p1");
    make_directory("p1/d");
    make_directory("sub/e");
    User::FileStatus before{};
    User::FileStatus after{};
    REQUIRE(Files::Stat(path("p1").c_str(), &before) == 0);
    REQUIRE(rename("p1/d", "sub/e") == 0);
    REQUIRE(Files::Stat(path("p1").c_str(), &after) == 0);
    if (hard_links)
    {
        REQUIRE(after.link_count + 1 == before.link_count);
    }
    REQUIRE(kind("sub/e") == User::ModeDirectory);
    REQUIRE(missing("p1/d"));
    make_directory("r1");
    make_directory("r2");
    REQUIRE(rename("r1", "r2") == 0);
    REQUIRE(missing("r1"));
    REQUIRE(kind("r2") == User::ModeDirectory);

    put("s2", "S");
    put("t2", "T");
    FeatherOS::Files::Table table{};
    const uint64_t target = Files::Open(&table, path("t2").c_str(), OpenRead);
    REQUIRE_FALSE(User::IsSyscallError(target));
    REQUIRE(rename("s2", "t2") == User::SyscallErrorBusy);
    REQUIRE(Files::Close(&table, target) == 0);
    REQUIRE(ReadWhole(path("t2").c_str()) == "T");
    REQUIRE(ReadWhole(path("s2").c_str()) == "S");
    const uint64_t source = Files::Open(&table, path("s2").c_str(), OpenRead);
    REQUIRE_FALSE(User::IsSyscallError(source));
    REQUIRE(rename("s2", "t2") == User::SyscallErrorBusy);
    REQUIRE(Files::Close(&table, source) == 0);
    REQUIRE(rename("s2", "t2") == 0);
    REQUIRE(ReadWhole(path("t2").c_str()) == "S");
}

} // namespace

#if defined(FEATHEROS_EXT2_TEST_IMAGE)
namespace
{

constexpr FeatherOS::Filesystem::MountMode ReadOnly = FeatherOS::Filesystem::MountMode::ReadOnly;

// One of the core's images at the end of the disk, as the MBR's entry; false without the image.
bool AppendExtImage(const char* path, uint32_t entry)
{
    std::FILE* image = std::fopen(path, "rb");
    if (image == nullptr)
    {
        return false;
    }
    const uint32_t start = static_cast<uint32_t>(disk.size());
    std::array<uint8_t, FeatherOS::BlockDevice::SectorSize> sector{};
    while (std::fread(sector.data(), 1, sector.size(), image) == sector.size())
    {
        disk.push_back(sector);
    }
    std::fclose(image);
    uint8_t* table = disk[0].data() + 446 + 16 * entry;
    table[4] = 0x83;
    WriteLittleEndian32(table + 8, start);
    WriteLittleEndian32(table + 12, static_cast<uint32_t>(disk.size()) - start);
    return true;
}

// The core's 1 KiB ext2 image as the disk's second partition, after a FAT16 one; false without
// the image.
bool AttachExt2Image()
{
    MakeFat16Disk("fat contents");
    return AppendExtImage(FEATHEROS_EXT2_TEST_IMAGE, 1);
}

// As make_ext2_image.sh names it.
std::string LongName()
{
    std::string name;
    while (name.size() < 200)
    {
        name += "a-long-name-";
    }
    name.resize(200);
    return name;
}

} // namespace

TEST_CASE("an ext2 partition after the FAT16 one mounts read-only by its partition name")
{
    using FeatherOS::VFS::MountResult;
    if (!AttachExt2Image())
    {
        SKIP("no ext2 image from mke2fs");
    }

    REQUIRE(FeatherOS::VFS::MountBlockDevice());
    REQUIRE(MountSource("/") == "vda1 fat16 rw");
    REQUIRE(FeatherOS::VFS::Mount("ext2", "vda1", "/mnt", ReadOnly) == MountResult::NotFound);
    REQUIRE(FeatherOS::VFS::Mount("ext2", "vda2", "/mnt", ReadOnly) == MountResult::Done);
    REQUIRE(MountSource("/mnt") == "vda2 ext2 ro");
    REQUIRE(ReadWhole("/mnt/hello.txt") == "Hello from ext2.\n");
    REQUIRE(ReadWhole("/mnt/a/b/c/deep.txt") == "deep\n");
    REQUIRE(FeatherOS::VFS::IsDirectory("/mnt/a/b"));
    FeatherOS::VFS::FileInfo info{};
    REQUIRE(FeatherOS::VFS::Stat("/mnt/big.txt", &info));
    REQUIRE(info.size == 448000);

    FeatherOS::VFS::FileHandle file{};
    REQUIRE(FeatherOS::VFS::IsReadOnly("/mnt/hello.txt"));
    REQUIRE(FeatherOS::VFS::IsReadOnly("/mnt/new.txt"));
    REQUIRE_FALSE(FeatherOS::VFS::Create("/mnt/new.txt", &file));
    REQUIRE(FeatherOS::VFS::LastError() == FeatherOS::User::SyscallErrorReadOnly);
    REQUIRE_FALSE(FeatherOS::VFS::MakeDirectory("/mnt/new"));
    REQUIRE_FALSE(FeatherOS::VFS::Remove("/mnt/hello.txt"));
    REQUIRE(FeatherOS::VFS::LastError() == FeatherOS::User::SyscallErrorReadOnly);
    REQUIRE_FALSE(FeatherOS::VFS::Open("/mnt/fast-link", &file));
    REQUIRE(FeatherOS::VFS::Open("/mnt/hello.txt", &file));
    REQUIRE_FALSE(FeatherOS::VFS::IsWritable(file));
    REQUIRE(FeatherOS::VFS::Unmount("/mnt") == MountResult::Busy);
    FeatherOS::VFS::Close(&file);
    REQUIRE(FeatherOS::VFS::Unmount("/mnt") == MountResult::Done);
    REQUIRE_FALSE(FeatherOS::VFS::Stat("/mnt/hello.txt", &info));

    // A disk without the ext2 partition forgets it.
    MakeFat16Disk("contents");
    REQUIRE(FeatherOS::VFS::MountBlockDevice());
    REQUIRE(FeatherOS::VFS::Mount("ext2", "vda2", "/mnt") == MountResult::UnknownType);
}

namespace
{

using Sector = std::array<uint8_t, FeatherOS::BlockDevice::SectorSize>;

// Superblock fields of the volume starting at sector start, which sit in the superblock's sector.
constexpr size_t SuperblockSector = 2;
constexpr size_t StateOffset = 0x3A;
constexpr size_t IncompatibleOffset = 0x60;
constexpr uint8_t NeedsRecoveryBit = 0x4;
constexpr uint8_t ErrorsBit = 2;

bool NeedsRecovery(uint32_t start)
{
    return (disk[start + SuperblockSector][IncompatibleOffset] & NeedsRecoveryBit) != 0;
}

uint8_t& State(uint32_t start)
{
    return disk[start + SuperblockSector][StateOffset];
}

std::vector<Sector> Copy(uint32_t start)
{
    return {disk.begin() + start, disk.end()};
}

bool AttachJournalledVolumes(uint32_t* ext3_start, uint32_t* ext4_start)
{
    if (!AttachExt2Image())
    {
        return false;
    }
    *ext3_start = static_cast<uint32_t>(disk.size());
    if (!AppendExtImage(FEATHEROS_EXT3_TEST_IMAGE, 2))
    {
        return false;
    }
    *ext4_start = static_cast<uint32_t>(disk.size());
    return AppendExtImage(FEATHEROS_EXT4_TEST_IMAGE, 3);
}

class CopiedPartition final : public FeatherDisk::BlockDevice
{
public:
    explicit CopiedPartition(std::vector<Sector> sectors) : sectors(std::move(sectors)) {}

    uint32_t SectorSize() const override
    {
        return FeatherOS::BlockDevice::SectorSize;
    }

    uint64_t SectorCount() const override
    {
        return sectors.size();
    }

    FeatherDisk::Status Read(uint64_t first_sector, std::span<uint8_t> buffer) override
    {
        for (size_t offset = 0; offset < buffer.size(); offset += SectorSize())
        {
            const uint64_t sector = first_sector + offset / SectorSize();
            if (sector >= sectors.size())
            {
                return std::unexpected(FeatherDisk::Error::Io);
            }
            std::memcpy(buffer.data() + offset, sectors[sector].data(), SectorSize());
        }
        return {};
    }

    FeatherDisk::Status Write(uint64_t first_sector, std::span<const uint8_t> data) override
    {
        for (size_t offset = 0; offset < data.size(); offset += SectorSize())
        {
            const uint64_t sector = first_sector + offset / SectorSize();
            if (sector >= sectors.size())
            {
                return std::unexpected(FeatherDisk::Error::Io);
            }
            std::memcpy(sectors[sector].data(), data.data() + offset, SectorSize());
        }
        return {};
    }

    FeatherDisk::Status Flush() override
    {
        return {};
    }

private:
    std::vector<Sector> sectors;
};

bool NoWorkingDirectory(const char* /*point*/)
{
    return false;
}

// What the core makes of a copy of the volume when it is asked to write: whether it takes writes
// and the contents of name in the root, or "<missing>".
struct CoreView
{
    bool writable;
    std::string contents;
};

CoreView MountCopy(FeatherDisk::FilesystemType& type, std::vector<Sector> sectors,
                   const char* name)
{
    CopiedPartition device(std::move(sectors));
    const FeatherDisk::Result<FeatherDisk::Filesystem*> mounted =
        type.Mount(device, {.read_only = false});
    REQUIRE(mounted);
    FeatherDisk::Filesystem& filesystem = **mounted;
    CoreView view{filesystem.GetCapabilities().writable, "<missing>"};
    if (const auto found = filesystem.Lookup(filesystem.Root(), name))
    {
        const auto attributes = filesystem.GetAttributes(*found);
        REQUIRE(attributes);
        std::string bytes(attributes->size, '\0');
        const auto read = filesystem.Read(
            *found, 0, {reinterpret_cast<uint8_t*>(bytes.data()), bytes.size()});
        REQUIRE(read);
        view.contents = bytes;
    }
    REQUIRE(type.Unmount(filesystem));
    return view;
}

} // namespace

TEST_CASE("ext3 and ext4 partitions register by their variant and mount read-write")
{
    using FeatherOS::VFS::MountResult;
    namespace User = FeatherOS::User;
    uint32_t ext3_start = 0;
    uint32_t ext4_start = 0;
    if (!AttachJournalledVolumes(&ext3_start, &ext4_start))
    {
        SKIP("no ext images from mke2fs");
    }

    REQUIRE(FeatherOS::VFS::MountBlockDevice());
    REQUIRE(MountSource("/") == "vda1 fat16 rw");
    REQUIRE(FeatherOS::VFS::Mount("ext2", "vda3", "/mnt") == MountResult::NotFound);
    REQUIRE(FeatherOS::VFS::Mount("ext4", "vda3", "/mnt") == MountResult::NotFound);
    REQUIRE(FeatherOS::VFS::Mount("ext3", "vda4", "/mnt") == MountResult::NotFound);
    REQUIRE(FeatherOS::VFS::Mount("ext4", "vda2", "/mnt") == MountResult::NotFound);
    for (const auto& [type, source, start] :
         {std::tuple{"ext3", "vda3", ext3_start}, std::tuple{"ext4", "vda4", ext4_start}})
    {
        INFO(type);
        const std::string description = std::string(source) + " " + type;
        REQUIRE_FALSE(NeedsRecovery(start));
        REQUIRE(FeatherOS::VFS::Mount(type, source, "/mnt") == MountResult::Done);
        REQUIRE(MountSource("/mnt") == description + " rw");
        REQUIRE(ReadWhole("/mnt/hello.txt") == "Hello from ext2.\n");
        FeatherOS::Files::Table table{};
        const uint64_t descriptor =
            FeatherOS::Files::Open(&table, "/mnt/new.txt", OpenWrite | OpenCreate);
        REQUIRE_FALSE(User::IsSyscallError(descriptor));
        REQUIRE(FeatherOS::Files::Write(&table, descriptor, "written\n", 8) == 8);
        REQUIRE(FeatherOS::Files::Close(&table, descriptor) == 0);
        REQUIRE(FeatherOS::Files::MakeDirectory("/mnt/dir") == 0);
        REQUIRE(FeatherOS::Files::Link("/mnt/new.txt", "/mnt/dir/hard.txt") == 0);
        REQUIRE(FeatherOS::Files::SymbolicLink("../new.txt", "/mnt/dir/soft") == 0);
        REQUIRE(NeedsRecovery(start));
        REQUIRE(FeatherOS::VFS::Unmount("/mnt") == MountResult::Done);
        REQUIRE_FALSE(NeedsRecovery(start));

        REQUIRE(FeatherOS::VFS::Mount(type, source, "/mnt") == MountResult::Done);
        REQUIRE(MountSource("/mnt") == description + " rw");
        REQUIRE(ReadWhole("/mnt/dir/hard.txt") == "written\n");
        char link_target[16] = {};
        REQUIRE(FeatherOS::Files::ReadLink("/mnt/dir/soft", link_target, sizeof(link_target)) ==
                10);
        REQUIRE(FeatherOS::VFS::Unmount("/mnt") == MountResult::Done);

        const std::vector<Sector> before = Copy(start);
        REQUIRE(FeatherOS::VFS::Mount(type, source, "/mnt", ReadOnly) == MountResult::Done);
        REQUIRE(MountSource("/mnt") == description + " ro");
        REQUIRE(ReadWhole("/mnt/dir/hard.txt") == "written\n");
        char target[16] = {};
        REQUIRE(FeatherOS::Files::ReadLink("/mnt/dir/soft", target, sizeof(target)) == 10);
        REQUIRE(FeatherOS::Files::Remove("/mnt/new.txt") == User::SyscallErrorReadOnly);
        REQUIRE(FeatherOS::Files::MakeDirectory("/mnt/more") == User::SyscallErrorReadOnly);
        REQUIRE(FeatherOS::VFS::Unmount("/mnt") == MountResult::Done);
        REQUIRE(Copy(start) == before);
    }
}

TEST_CASE("a journalled volume the core refuses mounts read-only and stays untouched")
{
    using FeatherOS::VFS::MountResult;
    namespace User = FeatherOS::User;
    uint32_t ext3_start = 0;
    uint32_t ext4_start = 0;
    if (!AttachJournalledVolumes(&ext3_start, &ext4_start))
    {
        SKIP("no ext images from mke2fs");
    }
    State(ext3_start) |= ErrorsBit;
    REQUIRE(FeatherOS::VFS::MountBlockDevice());
    const std::vector<Sector> before = Copy(ext3_start);

    REQUIRE(FeatherOS::VFS::Mount("ext3", "vda3", "/mnt") == MountResult::Done);
    REQUIRE(MountSource("/mnt") == "vda3 ext3 ro");
    FeatherOS::Files::Table table{};
    REQUIRE(FeatherOS::Files::Open(&table, "/mnt/new.txt", OpenWrite | OpenCreate) ==
            User::SyscallErrorReadOnly);
    REQUIRE(ReadWhole("/mnt/hello.txt") == "Hello from ext2.\n");
    REQUIRE(FeatherOS::VFS::Unmount("/mnt") == MountResult::Done);
    REQUIRE(Copy(ext3_start) == before);
}

TEST_CASE("the periodic sync commits a journalled change a stop would otherwise lose")
{
    using FeatherOS::VFS::MountResult;
    uint32_t ext3_start = 0;
    uint32_t ext4_start = 0;
    if (!AttachJournalledVolumes(&ext3_start, &ext4_start))
    {
        SKIP("no ext images from mke2fs");
    }
    REQUIRE(FeatherOS::VFS::MountBlockDevice());
    REQUIRE(FeatherOS::VFS::Mount("ext4", "vda4", "/mnt") == MountResult::Done);
    WriteAndClose("/mnt/p.txt", "periodic\n");
    const std::vector<Sector> before_pass = Copy(ext4_start);
    REQUIRE(FeatherOS::Files::SyncPeriodic() == 0);
    const std::vector<Sector> after_pass = Copy(ext4_start);

    const CoreView lost = MountCopy(FeatherDisk::Ext::Ext4Type(), before_pass, "p.txt");
    REQUIRE(lost.writable);
    REQUIRE(lost.contents == "<missing>");
    const CoreView kept = MountCopy(FeatherDisk::Ext::Ext4Type(), after_pass, "p.txt");
    REQUIRE(kept.writable);
    REQUIRE(kept.contents == "periodic\n");
    REQUIRE(FeatherOS::VFS::Unmount("/mnt") == MountResult::Done);
}

TEST_CASE("DetachAll closes journals and keeps the mounts")
{
    using FeatherOS::VFS::MountResult;
    uint32_t ext3_start = 0;
    uint32_t ext4_start = 0;
    if (!AttachJournalledVolumes(&ext3_start, &ext4_start))
    {
        SKIP("no ext images from mke2fs");
    }
    REQUIRE(FeatherOS::VFS::MountBlockDevice());
    REQUIRE(FeatherOS::VFS::Mount("ext4", "vda4", "/mnt") == MountResult::Done);
    WriteAndClose("/mnt/p.txt", "detached\n");
    REQUIRE(NeedsRecovery(ext4_start));

    REQUIRE(FeatherOS::VFS::DetachAll());
    REQUIRE_FALSE(NeedsRecovery(ext4_start));
    REQUIRE(MountSource("/mnt") == "vda4 ext4 rw");
    REQUIRE(MountSource("/") == "vda1 fat16 rw");
    REQUIRE(ReadWhole("/mnt/p.txt") == "detached\n");
    const std::vector<Sector> closed = Copy(ext4_start);
    REQUIRE(FeatherOS::VFS::DetachAll());
    REQUIRE(Copy(ext4_start) == closed);
    REQUIRE(FeatherOS::VFS::Unmount("/mnt") == MountResult::Done);
    REQUIRE(Copy(ext4_start) == closed);
}

namespace
{

std::string RootDescription();

void SwapPartitionEntries(size_t first, size_t second)
{
    uint8_t* table = disk[0].data() + 446;
    std::array<uint8_t, 16> saved{};
    std::memcpy(saved.data(), table + 16 * first, saved.size());
    std::memcpy(table + 16 * first, table + 16 * second, saved.size());
    std::memcpy(table + 16 * second, saved.data(), saved.size());
}

} // namespace

TEST_CASE("root= puts an ext4 volume at /, written, synced and closed like any mount")
{
    using FeatherOS::VFS::MountResult;
    uint32_t ext3_start = 0;
    uint32_t ext4_start = 0;
    if (!AttachJournalledVolumes(&ext3_start, &ext4_start))
    {
        SKIP("no ext images from mke2fs");
    }

    bool honored = false;
    REQUIRE(FeatherOS::VFS::MountBlockDevice(4, &honored));
    REQUIRE(honored);
    REQUIRE(RootDescription() == "vda4 ext4 rw");
    REQUIRE(MountSource("/") == "vda4 ext4 rw");
    REQUIRE(ReadWhole("/hello.txt") == "Hello from ext2.\n");
    REQUIRE(FeatherOS::VFS::Mount("ext4", "vda4", "/mnt") == MountResult::Busy);
    REQUIRE(FeatherOS::VFS::Mount("fat16", "vda1", "/fat") == MountResult::Done);
    REQUIRE(ReadWhole("/fat/BLOCK.TXT") == "fat contents");
    REQUIRE(FeatherOS::VFS::Unmount("/fat") == MountResult::Done);

    WriteAndClose("/root.txt", "on the root\n");
    REQUIRE(NeedsRecovery(ext4_start));
    const std::vector<Sector> before_pass = Copy(ext4_start);
    REQUIRE(FeatherOS::Files::SyncPeriodic() == 0);
    const CoreView lost = MountCopy(FeatherDisk::Ext::Ext4Type(), before_pass, "root.txt");
    REQUIRE(lost.contents == "<missing>");
    const CoreView kept =
        MountCopy(FeatherDisk::Ext::Ext4Type(), Copy(ext4_start), "root.txt");
    REQUIRE(kept.contents == "on the root\n");

    REQUIRE(FeatherOS::VFS::DetachAll());
    REQUIRE_FALSE(NeedsRecovery(ext4_start));
    REQUIRE(MountSource("/") == "vda4 ext4 rw");
    REQUIRE(ReadWhole("/root.txt") == "on the root\n");
}

TEST_CASE("root= replays the journal of an ext3 root that needs recovery")
{
    uint32_t ext3_start = 0;
    uint32_t ext4_start = 0;
    if (!AttachJournalledVolumes(&ext3_start, &ext4_start))
    {
        SKIP("no ext images from mke2fs");
    }
    REQUIRE(FeatherOS::VFS::MountBlockDevice(3));
    REQUIRE(RootDescription() == "vda3 ext3 rw");
    WriteAndClose("/lost.txt", "kept by the journal\n");
    REQUIRE(FeatherOS::Files::SyncPeriodic() == 0);
    const std::vector<Sector> crashed = Copy(ext3_start);
    REQUIRE(FeatherOS::VFS::DetachAll());
    REQUIRE_FALSE(NeedsRecovery(ext3_start));

    std::copy(crashed.begin(), crashed.end(), disk.begin() + ext3_start);
    REQUIRE(NeedsRecovery(ext3_start));
    REQUIRE(FeatherOS::VFS::MountBlockDevice(3));
    REQUIRE(RootDescription() == "vda3 ext3 rw");
    REQUIRE(ReadWhole("/lost.txt") == "kept by the journal\n");
}

TEST_CASE("without root= the lowest-numbered of the four types is the root")
{
    uint32_t ext3_start = 0;
    uint32_t ext4_start = 0;
    if (!AttachJournalledVolumes(&ext3_start, &ext4_start))
    {
        SKIP("no ext images from mke2fs");
    }
    SwapPartitionEntries(0, 3);
    bool honored = false;
    REQUIRE(FeatherOS::VFS::MountBlockDevice(0, &honored));
    REQUIRE(honored);
    REQUIRE(RootDescription() == "vda1 ext4 rw");
    REQUIRE(ReadWhole("/hello.txt") == "Hello from ext2.\n");

    REQUIRE(FeatherOS::VFS::MountBlockDevice(4, &honored));
    REQUIRE(honored);
    REQUIRE(RootDescription() == "vda4 fat16 rw");
    REQUIRE(FeatherOS::VFS::Mount("ext4", "vda1", "/mnt") ==
            FeatherOS::VFS::MountResult::Done);
    REQUIRE(FeatherOS::VFS::Unmount("/mnt") == FeatherOS::VFS::MountResult::Done);

    SwapPartitionEntries(0, 3);
    SwapPartitionEntries(0, 2);
    REQUIRE(FeatherOS::VFS::MountBlockDevice());
    REQUIRE(RootDescription() == "vda1 ext3 rw");
}

TEST_CASE("a root= that names no usable partition falls back to the default root")
{
    uint32_t ext3_start = 0;
    uint32_t ext4_start = 0;
    if (!AttachJournalledVolumes(&ext3_start, &ext4_start))
    {
        SKIP("no ext images from mke2fs");
    }
    bool honored = true;
    REQUIRE(FeatherOS::VFS::MountBlockDevice(9, &honored));
    REQUIRE_FALSE(honored);
    REQUIRE(RootDescription() == "vda1 fat16 rw");
    REQUIRE(FeatherOS::VFS::MountBlockDevice(5, &honored));
    REQUIRE_FALSE(honored);
    REQUIRE(RootDescription() == "vda1 fat16 rw");

    REQUIRE(FeatherOS::VFS::MountBlockDevice(2, &honored));
    REQUIRE(honored);
    REQUIRE(RootDescription() == "vda2 ext2 rw");
    REQUIRE(FeatherOS::VFS::MountBlockDevice(0, &honored));
    REQUIRE(honored);
    REQUIRE(RootDescription() == "vda1 fat16 rw");
}

TEST_CASE("an ext4 root the core refuses to write is mounted read-only")
{
    uint32_t ext3_start = 0;
    uint32_t ext4_start = 0;
    if (!AttachJournalledVolumes(&ext3_start, &ext4_start))
    {
        SKIP("no ext images from mke2fs");
    }
    State(ext3_start) |= ErrorsBit;
    const std::vector<Sector> before = Copy(ext3_start);
    bool honored = false;
    REQUIRE(FeatherOS::VFS::MountBlockDevice(3, &honored));
    REQUIRE(honored);
    REQUIRE(RootDescription() == "vda3 ext3 ro");
    FeatherOS::VFS::FileHandle file{};
    REQUIRE_FALSE(FeatherOS::VFS::Create("/new.txt", &file));
    REQUIRE(FeatherOS::VFS::LastError() == FeatherOS::User::SyscallErrorReadOnly);
    REQUIRE(FeatherOS::VFS::DetachAll());
    REQUIRE(Copy(ext3_start) == before);
}

TEST_CASE("a failed unmount keeps the mount; an aborted journal then unmounts")
{
    using FeatherOS::VFS::MountResult;
    namespace User = FeatherOS::User;
    uint32_t ext3_start = 0;
    uint32_t ext4_start = 0;
    if (!AttachJournalledVolumes(&ext3_start, &ext4_start))
    {
        SKIP("no ext images from mke2fs");
    }
    REQUIRE(FeatherOS::VFS::MountBlockDevice());
    REQUIRE(FeatherOS::VFS::Mount("ext4", "vda4", "/mnt") == MountResult::Done);
    FeatherOS::VFS::FileHandle file{};
    REQUIRE(FeatherOS::VFS::Open("/mnt/hello.txt", &file));
    REQUIRE(FeatherOS::VFS::Unmount("/mnt") == MountResult::Busy);
    FeatherOS::VFS::Close(&file);

    WriteAndClose("/mnt/p.txt", "lost\n");
    const WriteFailureGuard guard;
    writes_fail = true;
    REQUIRE(FeatherOS::Files::Unmount("/mnt", NoWorkingDirectory) == User::SyscallErrorInputOutput);
    REQUIRE(MountSource("/mnt") == "vda4 ext4 rw");
    REQUIRE(FeatherOS::VFS::Unmount("/mnt") == MountResult::Done);
    REQUIRE(MountSource("/mnt").empty());
    writes_fail = false;
    REQUIRE(NeedsRecovery(ext4_start));
}

TEST_CASE("ext2 attributes reach stat and directory entries through the core driver")
{
    using FeatherOS::User::FileStatus;
    using FeatherOS::User::FileType;
    namespace User = FeatherOS::User;
    if (!AttachExt2Image())
    {
        SKIP("no ext2 image from mke2fs");
    }
    REQUIRE(FeatherOS::VFS::MountBlockDevice());
    REQUIRE(FeatherOS::VFS::Mount("ext2", "vda2", "/mnt", ReadOnly) ==
            FeatherOS::VFS::MountResult::Done);

    FileStatus status{};
    REQUIRE(FeatherOS::Files::Stat("/mnt/hello.txt", &status) == 0);
    REQUIRE(status.mode == (User::ModeRegular | 0640));
    REQUIRE(status.type == FileType::Regular);
    REQUIRE(status.link_count == 2);
    REQUIRE(status.owner == 100000);
    REQUIRE(status.group == 4242);
    REQUIRE(status.size == 17);
    REQUIRE(status.access_time.seconds == 1600000000);
    REQUIRE(status.modify_time.seconds == 1577934245);
    REQUIRE(status.change_time.seconds == 1614834367);
    REQUIRE(status.read_only);
    const uint64_t hello_node = status.node;
    const uint64_t ext2_device = status.device;
    REQUIRE(FeatherOS::Files::Stat("/mnt/hard.txt", &status) == 0);
    REQUIRE(status.node == hello_node);
    REQUIRE(FeatherOS::Files::Stat("/mnt", &status) == 0);
    REQUIRE(status.mode == (User::ModeDirectory | 0755));
    REQUIRE(status.node == 2);
    REQUIRE(status.size == 1024);
    REQUIRE(FeatherOS::Files::Stat("/mnt/tool", &status) == 0);
    REQUIRE(status.mode == (User::ModeRegular | 04755));
    REQUIRE(FeatherOS::Files::LinkStat("/mnt/fast-link", &status) == 0);
    REQUIRE(status.mode == (User::ModeSymbolicLink | 0777));
    REQUIRE(status.type == FileType::SymbolicLink);
    REQUIRE(status.size == 14);
    // The core does not say what kind of special node a FIFO is.
    REQUIRE(FeatherOS::Files::Stat("/mnt/pipe", &status) == 0);
    REQUIRE(status.mode == 0644);
    REQUIRE(status.type == FileType::Unknown);
    REQUIRE(FeatherOS::Files::Stat("/BLOCK.TXT", &status) == 0);
    REQUIRE(status.device != ext2_device);

    FeatherOS::Files::Table table{};
    const uint64_t descriptor =
        FeatherOS::Files::Open(&table, "/mnt/hello.txt", FeatherOS::User::OpenRead);
    REQUIRE(FeatherOS::Files::StatDescriptor(&table, descriptor, &status) == 0);
    REQUIRE(status.node == hello_node);
    REQUIRE(status.device == ext2_device);
    REQUIRE(status.owner == 100000);
    REQUIRE(status.size == 17);
    FeatherOS::Files::CloseAll(&table);

    std::map<std::string, User::DirectoryEntry> entries;
    User::DirectoryEntry entry{};
    for (uint64_t index = 0; FeatherOS::Files::ReadDirectory("/mnt", index, &entry) == 0; ++index)
    {
        entries[entry.name] = entry;
    }
    REQUIRE(entries.at("a").type == User::ModeDirectory >> User::EntryTypeShift);
    REQUIRE(entries.at("hello.txt").type == User::ModeRegular >> User::EntryTypeShift);
    REQUIRE(entries.at("hello.txt").node == hello_node);
    REQUIRE(entries.at("hello.txt").size == 17);
    REQUIRE(entries.at("fast-link").type == User::ModeSymbolicLink >> User::EntryTypeShift);
    REQUIRE(entries.at("pipe").type == 0);
    REQUIRE(FeatherOS::VFS::Unmount("/mnt") == FeatherOS::VFS::MountResult::Done);
}

TEST_CASE("ext2 links are followed by the VFS walk, into and out of other mounts")
{
    namespace User = FeatherOS::User;
    using FeatherOS::User::FileType;
    if (!AttachExt2Image())
    {
        SKIP("no ext2 image from mke2fs");
    }
    REQUIRE(FeatherOS::VFS::MountBlockDevice());
    REQUIRE(FeatherOS::VFS::Mount("ext2", "vda2", "/mnt", ReadOnly) ==
            FeatherOS::VFS::MountResult::Done);
    FeatherOS::Tmpfs::Reset();
    FeatherOS::Files::Table table{};

    // fast-link holds "a/b/c/deep.txt" in its inode; slow-link a 64-byte target that does not
    // exist, in a block of its own.
    User::FileStatus status{};
    REQUIRE(FeatherOS::Files::Stat("/mnt/fast-link", &status) == 0);
    REQUIRE(status.type == FileType::Regular);
    REQUIRE(status.size == 5);
    uint64_t descriptor = FeatherOS::Files::Open(&table, "/mnt/fast-link", User::OpenRead);
    char contents[16] = {};
    REQUIRE(FeatherOS::Files::Read(&table, descriptor, contents, sizeof(contents)) == 5);
    REQUIRE(std::string(contents, 5) == "deep\n");
    REQUIRE(FeatherOS::Files::Close(&table, descriptor) == 0);
    REQUIRE(FeatherOS::Files::Open(&table, "/mnt/fast-link", User::OpenRead | User::OpenNoFollow) ==
            User::SyscallErrorLoop);
    REQUIRE(FeatherOS::Files::Open(&table, "/mnt/slow-link", User::OpenRead) ==
            User::SyscallErrorNotFound);
    char target[User::MaximumPathLength];
    const std::string slow_target =
        "a/b/c/a-symbolic-link-target-that-is-longer-than-sixty-bytes.txt";
    REQUIRE(FeatherOS::Files::ReadLink("/mnt/slow-link", target, sizeof(target)) ==
            slow_target.size());
    REQUIRE(std::string(target, slow_target.size()) == slow_target);
    REQUIRE(FeatherOS::Files::LinkStat("/mnt/slow-link", &status) == 0);
    REQUIRE(status.size == slow_target.size());

    // From /tmp into the ext2 mount and through its own link, and out again with "..".
    REQUIRE(FeatherOS::Files::SymbolicLink("/mnt/fast-link", "/tmp/to-ext2") == 0);
    REQUIRE(FeatherOS::Files::SymbolicLink("../mnt/a/b/c/..", "/tmp/up") == 0);
    REQUIRE(FeatherOS::Files::ResolvePath("/tmp/to-ext2", target, sizeof(target)) == 19);
    REQUIRE(std::string(target) == "/mnt/a/b/c/deep.txt");
    REQUIRE(FeatherOS::Files::ResolvePath("/tmp/up/../../../tmp", target, sizeof(target)) == 4);
    REQUIRE(std::string(target) == "/tmp");
    descriptor = FeatherOS::Files::Open(&table, "/tmp/to-ext2", User::OpenRead);
    REQUIRE(FeatherOS::Files::Read(&table, descriptor, contents, sizeof(contents)) == 5);
    REQUIRE(FeatherOS::Files::Close(&table, descriptor) == 0);
    char working_directory[User::MaximumPathLength] = "/";
    REQUIRE(FeatherOS::Files::ChangeDirectory("/tmp/up", working_directory) == 0);
    REQUIRE(std::string(working_directory) == "/mnt/a/b");

    // This ext2 mount is read-only, and FAT16 holds no links at all.
    REQUIRE(FeatherOS::Files::SymbolicLink("x", "/mnt/new-link") == User::SyscallErrorReadOnly);
    REQUIRE(FeatherOS::Files::Link("/mnt/hello.txt", "/mnt/hard2") == User::SyscallErrorReadOnly);
    REQUIRE(FeatherOS::Files::Remove("/mnt/fast-link") == User::SyscallErrorReadOnly);
    REQUIRE(FeatherOS::Files::SymbolicLink("x", "/fat-link") == User::SyscallErrorNotPermitted);
    REQUIRE(FeatherOS::Files::Link("/BLOCK.TXT", "/BLOCK2.TXT") == User::SyscallErrorNotPermitted);
    REQUIRE(FeatherOS::Files::Link("/mnt/hello.txt", "/tmp/hard") ==
            User::SyscallErrorCrossDevice);
    REQUIRE(FeatherOS::Files::Stat("/fat-link", &status) == User::SyscallErrorNotFound);

    FeatherOS::Tmpfs::Reset();
    REQUIRE(FeatherOS::VFS::Unmount("/mnt") == FeatherOS::VFS::MountResult::Done);
}

TEST_CASE("ext2 names of 200 bytes and paths near PATH_MAX pass through the VFS whole")
{
    if (!AttachExt2Image())
    {
        SKIP("no ext2 image from mke2fs");
    }
    REQUIRE(FeatherOS::VFS::MountBlockDevice());
    REQUIRE(FeatherOS::VFS::Mount("ext2", "vda2", "/mnt") == FeatherOS::VFS::MountResult::Done);
    const std::string name = LongName();
    REQUIRE(Sorted(ListDirectory("/mnt/a")) == std::vector<std::string>{name + "/", "b/"});
    std::string path = "/mnt/a";
    for (int level = 0; level < 5; ++level)
    {
        path += "/" + name;
    }
    REQUIRE(path.size() == 1011);
    REQUIRE(path.size() < FeatherOS::User::MaximumPathLength);
    REQUIRE(ReadWhole(path.c_str()) == "far\n");
    FeatherOS::User::FileStatus status{};
    REQUIRE(FeatherOS::Files::Stat(path.c_str(), &status) == 0);
    REQUIRE(status.size == 4);
    FeatherOS::User::DirectoryEntry entry{};
    const std::string directory = path.substr(0, path.size() - name.size() - 1);
    REQUIRE(FeatherOS::Files::ReadDirectory(directory.c_str(), 0, &entry) == 0);
    REQUIRE(std::string(entry.name) == name);
    REQUIRE(FeatherOS::VFS::Unmount("/mnt") == FeatherOS::VFS::MountResult::Done);
}

TEST_CASE("an ext2 partition mounted read-write takes changes through the file syscalls")
{
    namespace User = FeatherOS::User;
    using FeatherOS::VFS::MountResult;
    if (!AttachExt2Image())
    {
        SKIP("no ext2 image from mke2fs");
    }
    REQUIRE(FeatherOS::VFS::MountBlockDevice());
    REQUIRE(FeatherOS::VFS::Mount("ext2", "vda2", "/mnt") == MountResult::Done);
    REQUIRE(MountSource("/mnt") == "vda2 ext2 rw");
    FeatherOS::Files::Table table{};

    uint64_t descriptor =
        FeatherOS::Files::Open(&table, "/mnt/new.txt", OpenWrite | OpenCreate);
    REQUIRE_FALSE(User::IsSyscallError(descriptor));
    REQUIRE(FeatherOS::Files::Write(&table, descriptor, "written\n", 8) == 8);
    REQUIRE(FeatherOS::Files::Close(&table, descriptor) == 0);
    REQUIRE(FeatherOS::Files::MakeDirectory("/mnt/dir") == 0);
    REQUIRE(FeatherOS::Files::Link("/mnt/new.txt", "/mnt/dir/hard.txt") == 0);
    REQUIRE(FeatherOS::Files::SymbolicLink("../new.txt", "/mnt/dir/soft") == 0);
    REQUIRE(FeatherOS::Files::Rename("/mnt/hello.txt", "/mnt/dir/hello.txt") == 0);
    User::FileStatus status{};
    REQUIRE(FeatherOS::Files::Stat("/mnt/dir/soft", &status) == 0);
    REQUIRE(status.link_count == 2);
    REQUIRE_FALSE(status.read_only);
    REQUIRE(ReadWhole("/mnt/dir/hard.txt") == "written\n");
    REQUIRE(ReadWhole("/mnt/dir/hello.txt") == "Hello from ext2.\n");

    // Linux's answers for "." and "..", and errors that come from the core.
    REQUIRE(FeatherOS::Files::RemoveDirectory("/mnt/dir/.") == User::SyscallErrorInvalidArgument);
    REQUIRE(FeatherOS::Files::RemoveDirectory("/mnt/dir/sub/..") == User::SyscallErrorNotFound);
    REQUIRE(FeatherOS::Files::MakeDirectory("/mnt/dir/sub") == 0);
    REQUIRE(FeatherOS::Files::RemoveDirectory("/mnt/dir/sub/..") == User::SyscallErrorNotEmpty);
    REQUIRE(FeatherOS::Files::RemoveDirectory("/mnt/dir/sub/./") ==
            User::SyscallErrorInvalidArgument);
    REQUIRE(FeatherOS::Files::Rename("/mnt/dir/.", "/mnt/elsewhere") == User::SyscallErrorBusy);
    REQUIRE(FeatherOS::Files::Rename("/mnt/new.txt", "/mnt/dir/..") == User::SyscallErrorBusy);
    REQUIRE(FeatherOS::Files::Rename("/mnt/missing/.", "/mnt/x") == User::SyscallErrorNotFound);
    REQUIRE(FeatherOS::Files::RemoveDirectory("/mnt/dir") == User::SyscallErrorNotEmpty);
    REQUIRE(FeatherOS::Files::Link("/mnt/dir", "/mnt/dir-link") == User::SyscallErrorNotPermitted);
    REQUIRE(FeatherOS::Files::Rename("/mnt/dir", "/mnt/dir/sub/inside") ==
            User::SyscallErrorInvalidArgument);
    REQUIRE(FeatherOS::Files::RemoveDirectory("/mnt/dir/sub") == 0);

    // A full volume: the write that fills it is short, and the next one fails with ENOSPC.
    descriptor = FeatherOS::Files::Open(&table, "/mnt/fill.bin", OpenWrite | OpenCreate);
    REQUIRE_FALSE(User::IsSyscallError(descriptor));
    const std::vector<char> chunk(1024 * 1024, 'f');
    uint64_t result = 0;
    for (int round = 0; round < 16; ++round)
    {
        result = FeatherOS::Files::Write(&table, descriptor, chunk.data(), chunk.size());
        if (User::IsSyscallError(result) || result < chunk.size())
        {
            break;
        }
    }
    REQUIRE(result < chunk.size());
    REQUIRE(FeatherOS::Files::Write(&table, descriptor, chunk.data(), chunk.size()) ==
            User::SyscallErrorNoSpace);
    REQUIRE(FeatherOS::Files::Close(&table, descriptor) == 0);
    REQUIRE(FeatherOS::Files::Remove("/mnt/fill.bin") == 0);

    // Unmounting syncs; mounted again read-only, the changes are there and nothing changes.
    REQUIRE(FeatherOS::VFS::Unmount("/mnt") == MountResult::Done);
    REQUIRE(FeatherOS::VFS::Mount("ext2", "vda2", "/mnt", ReadOnly) == MountResult::Done);
    REQUIRE(MountSource("/mnt") == "vda2 ext2 ro");
    REQUIRE(ReadWhole("/mnt/dir/hard.txt") == "written\n");
    REQUIRE(ReadWhole("/mnt/dir/hello.txt") == "Hello from ext2.\n");
    char target[16] = {};
    REQUIRE(FeatherOS::Files::ReadLink("/mnt/dir/soft", target, sizeof(target)) == 10);
    REQUIRE(std::string(target, 10) == "../new.txt");
    REQUIRE(FeatherOS::Files::Stat("/mnt/hello.txt", &status) == User::SyscallErrorNotFound);
    REQUIRE(FeatherOS::Files::Remove("/mnt/new.txt") == User::SyscallErrorReadOnly);
    REQUIRE(FeatherOS::Files::Open(&table, "/mnt/new.txt", OpenWrite) ==
            User::SyscallErrorReadOnly);
    REQUIRE(FeatherOS::Files::Rename("/mnt/dir/.", "/mnt/x") == User::SyscallErrorBusy);
    REQUIRE(FeatherOS::VFS::Unmount("/mnt") == MountResult::Done);
}

namespace
{

// The ext2 volume as the disk's first partition and FAT16 as its second, as on the QEMU test
// disk; the first sector of the ext2 partition, or 0 without the image.
uint32_t AttachExt2Root()
{
    if (!AttachExt2Image())
    {
        return 0;
    }
    uint8_t* table = disk[0].data() + 446;
    std::array<uint8_t, 16> first{};
    std::memcpy(first.data(), table, first.size());
    std::memmove(table, table + 16, first.size());
    std::memcpy(table + 16, first.data(), first.size());
    return static_cast<uint32_t>(table[8] | table[9] << 8 | table[10] << 16 | table[11] << 24);
}

std::string RootDescription()
{
    const char* type = nullptr;
    const char* source = nullptr;
    bool read_only = false;
    REQUIRE(FeatherOS::VFS::DescribeRoot(&type, &source, &read_only));
    return std::string(source) + " " + type + (read_only ? " ro" : " rw");
}

} // namespace

TEST_CASE("the first partition's ext2 volume becomes the root, read-only if it refuses writes")
{
    using FeatherOS::VFS::MountResult;
    uint32_t ext2_start = AttachExt2Root();
    if (ext2_start == 0)
    {
        SKIP("no ext2 image from mke2fs");
    }

    // s_state, 58 bytes into the superblock 1024 bytes into the volume: not cleanly unmounted.
    disk[ext2_start + 2][58] = 0;
    REQUIRE(FeatherOS::VFS::MountBlockDevice());
    REQUIRE(RootDescription() == "vda1 ext2 ro");
    REQUIRE(MountSource("/") == "vda1 ext2 ro");
    REQUIRE(ReadWhole("/hello.txt") == "Hello from ext2.\n");
    FeatherOS::VFS::FileHandle file{};
    REQUIRE_FALSE(FeatherOS::VFS::Create("/new.txt", &file));
    REQUIRE(FeatherOS::VFS::LastError() == FeatherOS::User::SyscallErrorReadOnly);

    ext2_start = AttachExt2Root();
    REQUIRE(FeatherOS::VFS::MountBlockDevice());
    REQUIRE(RootDescription() == "vda1 ext2 rw");
    REQUIRE(ReadWhole("/hello.txt") == "Hello from ext2.\n");
    REQUIRE(FeatherOS::VFS::Create("/new.txt", &file));
    uint64_t written = 0;
    REQUIRE(FeatherOS::VFS::Write(&file, "root\n", 5, &written));
    FeatherOS::VFS::Close(&file);
    REQUIRE(disk[ext2_start + 2][58] == 0);
    REQUIRE(FeatherOS::Files::SyncAll() == 0);
    REQUIRE(disk[ext2_start + 2][58] == 1);
    REQUIRE(ReadWhole("/new.txt") == "root\n");

    REQUIRE(FeatherOS::VFS::Mount("fat16", "vda1", "/mnt") == MountResult::NotFound);
    REQUIRE(FeatherOS::VFS::Mount("ext2", "vda1", "/mnt") == MountResult::Busy);
    REQUIRE(FeatherOS::VFS::Mount("fat16", "vda2", "/mnt") == MountResult::Done);
    REQUIRE(MountSource("/mnt") == "vda2 fat16 rw");
    REQUIRE(ReadWhole("/mnt/BLOCK.TXT") == "fat contents");
    REQUIRE(FeatherOS::VFS::Unmount("/mnt") == MountResult::Done);

    // The core read-only again, so changing the disk below it writes nothing; then a FAT16 disk
    // gives a FAT16 root again.
    REQUIRE(FeatherOS::Filesystem::Drivers::MountExt2());
    MakeFat16Disk("contents");
    REQUIRE(FeatherOS::VFS::MountBlockDevice());
    REQUIRE(RootDescription() == "vda1 fat16 rw");
    REQUIRE(FeatherOS::VFS::Mount("ext2", "vda2", "/mnt") == MountResult::UnknownType);
}

TEST_CASE("an idle periodic sync writes nothing")
{
    using FeatherOS::VFS::MountResult;
    const uint32_t ext2_start = AttachExt2Root();
    uint32_t ext4_start = 0;
    if (ext2_start == 0 || !AppendExtImage(FEATHEROS_EXT3_TEST_IMAGE, 2))
    {
        SKIP("no ext images from mke2fs");
    }
    ext4_start = static_cast<uint32_t>(disk.size());
    REQUIRE(AppendExtImage(FEATHEROS_EXT4_TEST_IMAGE, 3));
    REQUIRE(FeatherOS::VFS::MountBlockDevice());
    REQUIRE(RootDescription() == "vda1 ext2 rw");
    REQUIRE(FeatherOS::VFS::Mount("ext4", "vda4", "/mnt") == MountResult::Done);
    REQUIRE(MountSource("/mnt") == "vda4 ext4 rw");
    WriteAndClose("/root.txt", "root\n");
    WriteAndClose("/mnt/p.txt", "periodic\n");

    REQUIRE(FeatherOS::Files::SyncPeriodic() == 0);
    const std::vector<Sector> settled = disk;
    for (int pass = 0; pass < 3; ++pass)
    {
        REQUIRE(FeatherOS::Files::SyncPeriodic() == 0);
        REQUIRE(disk == settled);
    }
    REQUIRE(State(ext2_start) == 1);
    REQUIRE(NeedsRecovery(ext4_start));
    REQUIRE(FeatherOS::VFS::Unmount("/mnt") == MountResult::Done);
    REQUIRE(FeatherOS::Files::SyncAll() == 0);
}

TEST_CASE("the periodic sync leaves a written ext2 root clean and consistent")
{
    const uint32_t start = AttachExt2Root();
    if (start == 0)
    {
        SKIP("no ext2 image from mke2fs");
    }
    REQUIRE(FeatherOS::VFS::MountBlockDevice());
    REQUIRE(RootDescription() == "vda1 ext2 rw");
    WriteAndClose("/new.txt", "periodic root\n");
    REQUIRE(State(start) == 0);
    const std::vector<Sector> unclean = Copy(start);
    REQUIRE(FeatherOS::Files::SyncPeriodic() == 0);
    REQUIRE(State(start) == 1);
    const std::vector<Sector> clean = Copy(start);

    const CoreView refused = MountCopy(FeatherDisk::Ext::Ext2Type(), unclean, "new.txt");
    REQUIRE_FALSE(refused.writable);
    const CoreView taken = MountCopy(FeatherDisk::Ext::Ext2Type(), clean, "new.txt");
    REQUIRE(taken.writable);
    REQUIRE(taken.contents == "periodic root\n");

    const WriteFailureGuard guard;
    std::vector<std::pair<uint64_t, uint8_t>> first_write;
    on_write = [&](uint64_t sector, const void* data) {
        if (first_write.empty())
        {
            first_write.emplace_back(sector, static_cast<const uint8_t*>(data)[StateOffset]);
        }
    };
    WriteAndClose("/second.txt", "second\n");
    REQUIRE(first_write.size() == 1);
    REQUIRE(first_write[0].first == start + SuperblockSector);
    REQUIRE(first_write[0].second == 0);
    on_write = nullptr;
    REQUIRE(FeatherOS::Files::SyncAll() == 0);
}
namespace
{

constexpr FeatherOS::Filesystem::MountMode ReadWrite = FeatherOS::Filesystem::MountMode::ReadWrite;

uint64_t CreateFile(const char* path)
{
    FeatherOS::Files::Table table{};
    const uint64_t descriptor = FeatherOS::Files::Open(&table, path, OpenWrite | OpenCreate);
    if (!FeatherOS::User::IsSyscallError(descriptor))
    {
        FeatherOS::Files::Close(&table, descriptor);
        return 0;
    }
    return descriptor;
}

bool MountExt4AtMnt(uint32_t* ext4_start)
{
    uint32_t ext3_start = 0;
    if (!AttachJournalledVolumes(&ext3_start, ext4_start))
    {
        return false;
    }
    REQUIRE(FeatherOS::VFS::MountBlockDevice());
    REQUIRE(FeatherOS::VFS::Mount("ext4", "vda4", "/mnt") == FeatherOS::VFS::MountResult::Done);
    return true;
}

} // namespace

TEST_CASE("remount switches an ext4 mount between read-only and read-write")
{
    using FeatherOS::VFS::MountResult;
    namespace User = FeatherOS::User;
    uint32_t start = 0;
    if (!MountExt4AtMnt(&start))
    {
        SKIP("no ext images from mke2fs");
    }
    WriteAndClose("/mnt/r.txt", "remount\n");
    REQUIRE(NeedsRecovery(start));

    REQUIRE(FeatherOS::Files::Remount("/mnt", ReadOnly) == 0);
    REQUIRE(MountSource("/mnt") == "vda4 ext4 ro");
    REQUIRE_FALSE(NeedsRecovery(start));
    REQUIRE(CreateFile("/mnt/new.txt") == User::SyscallErrorReadOnly);
    REQUIRE(ReadWhole("/mnt/r.txt") == "remount\n");
    REQUIRE(ReadWhole("/mnt/hello.txt") == "Hello from ext2.\n");

    const std::vector<Sector> snapshot = disk;
    for (int pass = 0; pass < 3; ++pass)
    {
        REQUIRE(FeatherOS::Files::SyncPeriodic() == 0);
    }
    REQUIRE(FeatherOS::Files::SyncAll() == 0);
    REQUIRE(FeatherOS::VFS::DetachAll());
    REQUIRE(FeatherOS::Files::Remount("/mnt", ReadOnly) == 0);
    REQUIRE(disk == snapshot);

    REQUIRE(FeatherOS::Files::Remount("/mnt", ReadWrite) == 0);
    REQUIRE(MountSource("/mnt") == "vda4 ext4 rw");
    REQUIRE(FeatherOS::Files::Remount("/mnt", ReadWrite) == 0);
    WriteAndClose("/mnt/w.txt", "again\n");
    REQUIRE(NeedsRecovery(start));
    REQUIRE(FeatherOS::VFS::Unmount("/mnt") == MountResult::Done);
    REQUIRE_FALSE(NeedsRecovery(start));
    REQUIRE(FeatherOS::VFS::Mount("ext4", "vda4", "/mnt", ReadOnly) == MountResult::Done);
    REQUIRE(ReadWhole("/mnt/r.txt") == "remount\n");
    REQUIRE(ReadWhole("/mnt/w.txt") == "again\n");
    REQUIRE(FeatherOS::VFS::Unmount("/mnt") == MountResult::Done);
}

namespace
{

// The root remounted read-only and back, with /etc as the root's own directory; /etc is the
// tmpfs again after.
void CheckRootRemount(const char* description, const std::function<bool()>& marked_clean)
{
    namespace User = FeatherOS::User;
    REQUIRE(RootDescription() == std::string(description) + " rw");
    // UseRootEtc reads the initramfs, which would otherwise be an earlier test's freed archive.
    const std::vector<uint8_t> archive = MakeTarArchive("etc/hosts", "127.0.0.1 localhost\n");
    REQUIRE(FeatherOS::Initramfs::Mount(archive.data(), archive.size()));
    REQUIRE(FeatherOS::VFS::UseRootEtc());
    WriteAndClose("/etc/kept.txt", "etc\n");
    WriteAndClose("/data.txt", "data\n");

    REQUIRE(FeatherOS::Files::Remount("/", ReadOnly) == 0);
    REQUIRE(RootDescription() == std::string(description) + " ro");
    REQUIRE(marked_clean());
    REQUIRE(CreateFile("/etc/new.txt") == User::SyscallErrorReadOnly);
    REQUIRE(CreateFile("/new.txt") == User::SyscallErrorReadOnly);
    REQUIRE(CreateFile("/tmp/new.txt") == 0);
    REQUIRE(FeatherOS::Files::Remove("/tmp/new.txt") == 0);
    REQUIRE(ReadWhole("/etc/kept.txt") == "etc\n");
    REQUIRE(ReadWhole("/data.txt") == "data\n");

    const std::vector<Sector> snapshot = disk;
    REQUIRE(FeatherOS::VFS::DetachAll());
    REQUIRE(disk == snapshot);

    REQUIRE(FeatherOS::Files::Remount("/", ReadWrite) == 0);
    REQUIRE(RootDescription() == std::string(description) + " rw");
    WriteAndClose("/etc/second.txt", "back\n");
    REQUIRE(ReadWhole("/etc/second.txt") == "back\n");
    REQUIRE(FeatherOS::VFS::DetachAll());
    REQUIRE(MountSource("/etc").empty());
    REQUIRE(FeatherOS::VFS::Mount("tmpfs", "etc", "/etc") == FeatherOS::VFS::MountResult::Done);
}

} // namespace

TEST_CASE("the ext2 root remounts read-only and back, and /etc follows it")
{
    const uint32_t start = AttachExt2Root();
    if (start == 0)
    {
        SKIP("no ext2 image from mke2fs");
    }
    REQUIRE(FeatherOS::VFS::MountBlockDevice());
    CheckRootRemount("vda1 ext2", [&] { return State(start) == 1; });
}

TEST_CASE("the ext4 root remounts read-only and back, and /etc follows it")
{
    uint32_t ext3_start = 0;
    uint32_t ext4_start = 0;
    if (!AttachJournalledVolumes(&ext3_start, &ext4_start))
    {
        SKIP("no ext images from mke2fs");
    }
    REQUIRE(FeatherOS::VFS::MountBlockDevice(4));
    CheckRootRemount("vda4 ext4", [&] { return !NeedsRecovery(ext4_start); });
}

TEST_CASE("remount refuses while a file on the mount is open for writing")
{
    using FeatherOS::VFS::MountResult;
    namespace User = FeatherOS::User;
    uint32_t start = 0;
    if (!MountExt4AtMnt(&start))
    {
        SKIP("no ext images from mke2fs");
    }
    FeatherOS::Files::Table table{};
    const uint64_t writer = FeatherOS::Files::Open(&table, "/mnt/f.txt", OpenWrite | OpenCreate);
    REQUIRE_FALSE(User::IsSyscallError(writer));
    REQUIRE(FeatherOS::Files::Write(&table, writer, "held\n", 5) == 5);

    const std::vector<Sector> snapshot = disk;
    REQUIRE(FeatherOS::Files::Remount("/mnt", ReadOnly) == User::SyscallErrorBusy);
    REQUIRE(disk == snapshot);
    REQUIRE(MountSource("/mnt") == "vda4 ext4 rw");
    REQUIRE(FeatherOS::Files::Write(&table, writer, "more\n", 5) == 5);
    REQUIRE(FeatherOS::Files::Close(&table, writer) == 0);

    const uint64_t reader = FeatherOS::Files::Open(&table, "/mnt/f.txt", OpenRead);
    REQUIRE_FALSE(User::IsSyscallError(reader));
    REQUIRE(FeatherOS::Files::Remount("/mnt", ReadOnly) == 0);
    REQUIRE(MountSource("/mnt") == "vda4 ext4 ro");
    char contents[16] = {};
    REQUIRE(FeatherOS::Files::Read(&table, reader, contents, sizeof(contents)) == 10);
    REQUIRE(std::string(contents, 10) == "held\nmore\n");
    REQUIRE(FeatherOS::Files::Remount("/mnt", ReadWrite) == 0);
    REQUIRE(FeatherOS::Files::Close(&table, reader) == 0);
    REQUIRE(FeatherOS::VFS::Unmount("/mnt") == MountResult::Done);
}

TEST_CASE("a remount to read-write that the core refuses gives EROFS and writes nothing")
{
    using FeatherOS::VFS::MountResult;
    namespace User = FeatherOS::User;
    uint32_t ext3_start = 0;
    uint32_t ext4_start = 0;
    if (!AttachJournalledVolumes(&ext3_start, &ext4_start))
    {
        SKIP("no ext images from mke2fs");
    }
    State(ext3_start) |= ErrorsBit;
    REQUIRE(FeatherOS::VFS::MountBlockDevice());
    REQUIRE(FeatherOS::VFS::Mount("ext3", "vda3", "/mnt") == MountResult::Done);
    REQUIRE(MountSource("/mnt") == "vda3 ext3 ro");
    std::vector<Sector> snapshot = disk;
    REQUIRE(FeatherOS::Files::Remount("/mnt", ReadWrite) == User::SyscallErrorReadOnly);
    REQUIRE(MountSource("/mnt") == "vda3 ext3 ro");
    REQUIRE(CreateFile("/mnt/new.txt") == User::SyscallErrorReadOnly);
    REQUIRE(FeatherOS::Files::Remount("/mnt", ReadWrite) == User::SyscallErrorReadOnly);
    REQUIRE(disk == snapshot);
    REQUIRE(FeatherOS::VFS::Unmount("/mnt") == MountResult::Done);

    // An ext2 volume that was not cleanly unmounted.
    State(DiskSectorCount) = 0;
    REQUIRE(FeatherOS::VFS::MountBlockDevice());
    REQUIRE(FeatherOS::VFS::Mount("ext2", "vda2", "/mnt") == MountResult::Done);
    REQUIRE(MountSource("/mnt") == "vda2 ext2 ro");
    snapshot = disk;
    REQUIRE(FeatherOS::Files::Remount("/mnt", ReadWrite) == User::SyscallErrorReadOnly);
    REQUIRE(MountSource("/mnt") == "vda2 ext2 ro");
    REQUIRE(disk == snapshot);
    REQUIRE(FeatherOS::VFS::Unmount("/mnt") == MountResult::Done);

    // A clean volume mounted read-only takes writes after a remount.
    REQUIRE(FeatherOS::VFS::MountBlockDevice());
    REQUIRE(FeatherOS::VFS::Mount("ext4", "vda4", "/mnt", ReadOnly) == MountResult::Done);
    REQUIRE(MountSource("/mnt") == "vda4 ext4 ro");
    REQUIRE(FeatherOS::Files::Remount("/mnt", ReadWrite) == 0);
    REQUIRE(MountSource("/mnt") == "vda4 ext4 rw");
    WriteAndClose("/mnt/now.txt", "writable\n");
    REQUIRE(FeatherOS::VFS::Unmount("/mnt") == MountResult::Done);
}

TEST_CASE("an aborted journal shows read-only in /proc/mounts and stays that way")
{
    using FeatherOS::VFS::MountResult;
    namespace User = FeatherOS::User;
    for (const bool abort_in_sync : {false, true})
    {
        INFO(std::string("abort in a periodic sync: ") + (abort_in_sync ? "yes" : "no"));
        uint32_t start = 0;
        if (!MountExt4AtMnt(&start))
        {
            SKIP("no ext images from mke2fs");
        }
        WriteAndClose("/mnt/kept.txt", "committed\n");
        REQUIRE(FeatherOS::Files::SyncPeriodic() == 0);
        WriteAndClose("/mnt/lost.txt", "running\n");
        {
            const WriteFailureGuard guard;
            writes_fail = true;
            if (abort_in_sync)
            {
                REQUIRE(FeatherOS::Files::SyncPeriodic() == User::SyscallErrorInputOutput);
            }
            else
            {
                // Changes fill the running transaction until one of them has to commit it.
                uint64_t result = 0;
                for (int index = 0; result == 0 && index < 4000; ++index)
                {
                    result = FeatherOS::Files::MakeDirectory(
                        ("/mnt/directory-" + std::to_string(index)).c_str());
                }
                REQUIRE(result != 0);
            }
            REQUIRE(MountSource("/mnt") == "vda4 ext4 ro");
            REQUIRE(CreateFile("/mnt/more.txt") == User::SyscallErrorReadOnly);
        }

        const uint32_t flushes = flush_count;
        REQUIRE(FeatherOS::Files::SyncPeriodic() == 0);
        REQUIRE(flush_count == flushes);
        const std::vector<Sector> snapshot = disk;
        REQUIRE(FeatherOS::Files::Remount("/mnt", ReadWrite) == User::SyscallErrorReadOnly);
        REQUIRE(MountSource("/mnt") == "vda4 ext4 ro");
        REQUIRE(FeatherOS::Files::Remount("/mnt", ReadOnly) == 0);
        REQUIRE(disk == snapshot);
        REQUIRE(NeedsRecovery(start));

        REQUIRE(FeatherOS::VFS::Unmount("/mnt") == MountResult::Done);
        REQUIRE(FeatherOS::VFS::Mount("ext4", "vda4", "/mnt") == MountResult::Done);
        REQUIRE(MountSource("/mnt") == "vda4 ext4 rw");
        REQUIRE(ReadWhole("/mnt/kept.txt") == "committed\n");
        REQUIRE(FeatherOS::VFS::Unmount("/mnt") == MountResult::Done);
    }
}

TEST_CASE("a failed close keeps the mount and shows it read-only")
{
    using FeatherOS::VFS::MountResult;
    namespace User = FeatherOS::User;
    uint32_t start = 0;
    if (!MountExt4AtMnt(&start))
    {
        SKIP("no ext images from mke2fs");
    }
    WriteAndClose("/mnt/p.txt", "pending\n");
    {
        const WriteFailureGuard guard;
        writes_fail = true;
        REQUIRE(FeatherOS::Files::Remount("/mnt", ReadOnly) == User::SyscallErrorInputOutput);
        REQUIRE(MountSource("/mnt") == "vda4 ext4 ro");
    }
    const std::vector<Sector> snapshot = disk;
    REQUIRE(FeatherOS::Files::Remount("/mnt", ReadOnly) == 0);
    REQUIRE(disk == snapshot);
    REQUIRE(NeedsRecovery(start));
    REQUIRE(FeatherOS::VFS::Unmount("/mnt") == MountResult::Done);
}

namespace
{

struct ReadFailureGuard
{
    ~ReadFailureGuard()
    {
        reads_fail = false;
    }
};

void CheckFailedReopen(const char* point)
{
    namespace User = FeatherOS::User;
    FeatherOS::Files::Table table{};
    {
        const ReadFailureGuard guard;
        reads_fail = true;
        REQUIRE(FeatherOS::Files::Remount(point, ReadWrite) == User::SyscallErrorInputOutput);
    }
    REQUIRE(MountSource(point) == "vda4 ext4 ro");
    const std::string file = std::string(point) + "/r.txt";
    REQUIRE(User::IsSyscallError(FeatherOS::Files::Open(&table, file.c_str(), User::OpenRead)));
    FeatherOS::VFS::FileHandle handle{};
    REQUIRE_FALSE(FeatherOS::VFS::Open(file.c_str(), &handle));
    REQUIRE(CreateFile((std::string(point) + "/new.txt").c_str()) == User::SyscallErrorReadOnly);
    REQUIRE(FeatherOS::Files::SyncAll() == 0);
    REQUIRE(FeatherOS::Files::SyncPeriodic() == 0);
}

} // namespace

TEST_CASE("a remount whose reopen fails leaves a mount that fails cleanly and reopens later")
{
    using FeatherOS::VFS::MountResult;
    uint32_t start = 0;
    if (!MountExt4AtMnt(&start))
    {
        SKIP("no ext images from mke2fs");
    }
    WriteAndClose("/mnt/r.txt", "remount\n");

    SECTION("after a read-only remount, then a second remount")
    {
        REQUIRE(FeatherOS::Files::Remount("/mnt", ReadOnly) == 0);
        CheckFailedReopen("/mnt");
        REQUIRE(FeatherOS::Files::Remount("/mnt", ReadWrite) == 0);
        REQUIRE(MountSource("/mnt") == "vda4 ext4 rw");
        REQUIRE(ReadWhole("/mnt/r.txt") == "remount\n");
        WriteAndClose("/mnt/w.txt", "again\n");
    }
    SECTION("after a read-only mount, then unmount and mount")
    {
        REQUIRE(FeatherOS::VFS::Unmount("/mnt") == MountResult::Done);
        REQUIRE(FeatherOS::VFS::Mount("ext4", "vda4", "/mnt", ReadOnly) == MountResult::Done);
        CheckFailedReopen("/mnt");
        REQUIRE(FeatherOS::VFS::Unmount("/mnt") == MountResult::Done);
        REQUIRE(FeatherOS::VFS::Mount("ext4", "vda4", "/mnt") == MountResult::Done);
        REQUIRE(MountSource("/mnt") == "vda4 ext4 rw");
        REQUIRE(ReadWhole("/mnt/r.txt") == "remount\n");
        WriteAndClose("/mnt/w.txt", "again\n");
    }
    REQUIRE(FeatherOS::VFS::Unmount("/mnt") == MountResult::Done);
    REQUIRE_FALSE(NeedsRecovery(start));
}

TEST_CASE("a remount to read-write that replays the journal waits for open files")
{
    using FeatherOS::VFS::MountResult;
    namespace User = FeatherOS::User;
    uint32_t start = 0;
    if (!MountExt4AtMnt(&start))
    {
        SKIP("no ext images from mke2fs");
    }
    WriteAndClose("/mnt/lost.txt", "replayed\n");
    REQUIRE(FeatherOS::Files::SyncPeriodic() == 0);
    const std::vector<Sector> crashed = Copy(start);
    REQUIRE(FeatherOS::VFS::Unmount("/mnt") == MountResult::Done);
    std::copy(crashed.begin(), crashed.end(), disk.begin() + start);
    REQUIRE(NeedsRecovery(start));

    REQUIRE(FeatherOS::VFS::MountBlockDevice());
    REQUIRE(FeatherOS::VFS::Mount("ext4", "vda4", "/mnt", ReadOnly) == MountResult::Done);
    FeatherOS::VFS::FileInfo info{};
    REQUIRE_FALSE(FeatherOS::VFS::Stat("/mnt/lost.txt", &info));
    const std::vector<Sector> snapshot = disk;

    FeatherOS::Files::Table table{};
    const uint64_t reader = FeatherOS::Files::Open(&table, "/mnt/hello.txt", OpenRead);
    REQUIRE_FALSE(User::IsSyscallError(reader));
    REQUIRE(FeatherOS::Files::Remount("/mnt", ReadWrite) == User::SyscallErrorBusy);
    REQUIRE(MountSource("/mnt") == "vda4 ext4 ro");
    REQUIRE(disk == snapshot);
    REQUIRE(FeatherOS::Files::Close(&table, reader) == 0);

    REQUIRE(FeatherOS::Files::Remount("/mnt", ReadWrite) == 0);
    REQUIRE(MountSource("/mnt") == "vda4 ext4 rw");
    REQUIRE(ReadWhole("/mnt/lost.txt") == "replayed\n");
    REQUIRE(FeatherOS::VFS::Unmount("/mnt") == MountResult::Done);
}

TEST_CASE("FAT16 remounts read-only with a flush and back")
{
    using FeatherOS::VFS::MountResult;
    namespace User = FeatherOS::User;
    uint32_t ext3_start = 0;
    uint32_t ext4_start = 0;
    if (!AttachJournalledVolumes(&ext3_start, &ext4_start))
    {
        SKIP("no ext images from mke2fs");
    }
    REQUIRE(FeatherOS::VFS::MountBlockDevice(4));
    REQUIRE(FeatherOS::VFS::Mount("fat16", "vda1", "/fat") == MountResult::Done);
    WriteAndClose("/fat/FIRST.TXT", "first");

    const uint32_t flushes = flush_count;
    REQUIRE(FeatherOS::Files::Remount("/fat", ReadOnly) == 0);
    REQUIRE(flush_count == flushes + 1);
    REQUIRE(MountSource("/fat") == "vda1 fat16 ro");
    REQUIRE(CreateFile("/fat/SECOND.TXT") == User::SyscallErrorReadOnly);
    REQUIRE(ReadWhole("/fat/FIRST.TXT") == "first");
    REQUIRE(ReadWhole("/fat/BLOCK.TXT") == "fat contents");

    REQUIRE(FeatherOS::Files::Remount("/fat", ReadWrite) == 0);
    REQUIRE(MountSource("/fat") == "vda1 fat16 rw");
    WriteAndClose("/fat/SECOND.TXT", "second");
    REQUIRE(ReadWhole("/fat/FIRST.TXT") == "first");
    REQUIRE(FeatherOS::VFS::Unmount("/fat") == MountResult::Done);
}

namespace
{

using Recorded = std::vector<std::pair<uint64_t, Sector>>;

// What the target name holds on a copy of base after each prefix of writes; "<missing>" for none.
std::set<std::string> TargetsAfterPrefixes(FeatherDisk::FilesystemType& type,
                                           const std::vector<Sector>& base, const Recorded& writes,
                                           const char* source, const char* target)
{
    std::set<std::string> seen;
    for (size_t count = 0; count <= writes.size(); ++count)
    {
        std::vector<Sector> copy = base;
        for (size_t index = 0; index < count; ++index)
        {
            copy[writes[index].first] = writes[index].second;
        }
        const std::string held = MountCopy(type, copy, target).contents;
        INFO("after " << count << " of " << writes.size() << " writes");
        if (held == "<missing>")
        {
            REQUIRE(MountCopy(type, copy, source).contents == "new");
        }
        else
        {
            REQUIRE((held == "old" || held == "new"));
        }
        seen.insert(held);
    }
    return seen;
}

struct UnmountGuard
{
    UnmountGuard() = default;
    UnmountGuard(const UnmountGuard&) = delete;
    UnmountGuard& operator=(const UnmountGuard&) = delete;

    ~UnmountGuard()
    {
        FeatherOS::VFS::Unmount("/mnt");
    }
};

void RecordWrites(Recorded* writes, uint32_t start)
{
    on_write = [writes, start](uint64_t sector, const void* data) {
        Sector copy;
        std::memcpy(copy.data(), data, copy.size());
        writes->emplace_back(sector - start, copy);
    };
}

} // namespace

TEST_CASE("a replacing rename on ext2 is one core call: the target is old or new after every write")
{
    using FeatherOS::VFS::MountResult;
    if (!AttachExt2Image())
    {
        SKIP("no ext2 image from mke2fs");
    }
    const uint32_t start = DiskSectorCount;
    const UnmountGuard unmounter;
    REQUIRE(FeatherOS::VFS::MountBlockDevice());
    REQUIRE(FeatherOS::VFS::Mount("ext2", "vda2", "/mnt") == MountResult::Done);
    WriteAndClose("/mnt/dst.txt", "old");
    WriteAndClose("/mnt/src.txt", "new");
    REQUIRE(FeatherOS::Files::SyncPeriodic() == 0);
    const std::vector<Sector> base = Copy(start);

    const WriteFailureGuard guard;
    Recorded writes;
    RecordWrites(&writes, start);
    REQUIRE(FeatherOS::Files::Rename("/mnt/src.txt", "/mnt/dst.txt") == 0);
    REQUIRE(FeatherOS::Files::SyncPeriodic() == 0);
    on_write = nullptr;
    REQUIRE(writes.size() > 1);

    const std::set<std::string> seen = TargetsAfterPrefixes(
        FeatherDisk::Ext::Ext2Type(), base, writes, "src.txt", "dst.txt");
    REQUIRE(seen == std::set<std::string>{"old", "new"});
}

TEST_CASE("a replacing rename on ext4 is one core call: the target is old or new after every write")
{
    const UnmountGuard unmounter;
    uint32_t start = 0;
    if (!MountExt4AtMnt(&start))
    {
        SKIP("no ext images from mke2fs");
    }
    WriteAndClose("/mnt/dst.txt", "old");
    WriteAndClose("/mnt/src.txt", "new");
    REQUIRE(FeatherOS::Files::SyncPeriodic() == 0);
    const std::vector<Sector> base = Copy(start);

    const WriteFailureGuard guard;
    Recorded writes;
    RecordWrites(&writes, start);
    REQUIRE(FeatherOS::Files::Rename("/mnt/src.txt", "/mnt/dst.txt") == 0);
    REQUIRE(FeatherOS::Files::SyncPeriodic() == 0);
    on_write = nullptr;
    REQUIRE(writes.size() > 1);

    const std::set<std::string> seen = TargetsAfterPrefixes(
        FeatherDisk::Ext::Ext4Type(), base, writes, "src.txt", "dst.txt");
    REQUIRE(seen == std::set<std::string>{"old", "new"});
}

TEST_CASE("FAT16's replacing rename may lose the target in a crash but never the source")
{
    MakeFat16Disk("contents");
    REQUIRE(FeatherOS::VFS::MountBlockDevice());
    WriteAndClose("/dst.txt", "old");
    WriteAndClose("/src.txt", "new");
    const std::vector<Sector> base = Copy(PartitionStart);

    const WriteFailureGuard guard;
    Recorded writes;
    RecordWrites(&writes, PartitionStart);
    REQUIRE(FeatherOS::Files::Rename("/src.txt", "/dst.txt") == 0);
    on_write = nullptr;
    REQUIRE(writes.size() > 1);

    const std::set<std::string> seen = TargetsAfterPrefixes(
        FeatherDisk::Fat::Fat16Type(), base, writes, "src.txt", "dst.txt");
    REQUIRE(seen.contains("old"));
    REQUIRE(seen.contains("new"));
    REQUIRE(seen.contains("<missing>"));
}

TEST_CASE("rename onto an existing name follows one set of rules on ext2, ext3, ext4, FAT16 and tmpfs")
{
    using FeatherOS::VFS::MountResult;
    const UnmountGuard unmounter;
    uint32_t ext4_start = 0;
    if (!MountExt4AtMnt(&ext4_start))
    {
        SKIP("no ext images from mke2fs");
    }
    SECTION("ext4")
    {
        CheckReplacementRules("/mnt/r", true, true);
    }
    SECTION("ext3")
    {
        REQUIRE(FeatherOS::VFS::Unmount("/mnt") == MountResult::Done);
        REQUIRE(FeatherOS::VFS::Mount("ext3", "vda3", "/mnt") == MountResult::Done);
        CheckReplacementRules("/mnt/r", true, true);
    }
    SECTION("ext2")
    {
        REQUIRE(FeatherOS::VFS::Unmount("/mnt") == MountResult::Done);
        REQUIRE(FeatherOS::VFS::Mount("ext2", "vda2", "/mnt") == MountResult::Done);
        CheckReplacementRules("/mnt/r", true, true);
        WriteAndClose("/mnt/r/old", "old");
        WriteAndClose("/mnt/r/new", "new");
        const WriteFailureGuard guard;
        writes_fail = true;
        REQUIRE(FeatherOS::Files::Rename("/mnt/r/new", "/mnt/r/old") ==
                FeatherOS::User::SyscallErrorInputOutput);
        writes_fail = false;
    }
    SECTION("FAT16")
    {
        CheckReplacementRules("/r", false, false);
    }
    SECTION("tmpfs")
    {
        FeatherOS::Tmpfs::Reset();
        CheckReplacementRules("/tmp/r", true, false);
    }
}

#endif

TEST_CASE("core errors become Linux's errno codes")
{
    namespace User = FeatherOS::User;
    using FeatherDisk::Error;
    using FeatherOS::Filesystem::SyscallError;
    const std::vector<std::pair<Error, uint64_t>> expected = {
        {Error::Io, User::SyscallErrorInputOutput},
        {Error::Corrupt, User::SyscallErrorInputOutput},
        {Error::NotFound, User::SyscallErrorNotFound},
        {Error::Exists, User::SyscallErrorExists},
        {Error::NotDirectory, User::SyscallErrorNotDirectory},
        {Error::IsDirectory, User::SyscallErrorIsDirectory},
        {Error::NotEmpty, User::SyscallErrorNotEmpty},
        {Error::NoSpace, User::SyscallErrorNoSpace},
        {Error::ReadOnly, User::SyscallErrorReadOnly},
        {Error::InvalidArgument, User::SyscallErrorInvalidArgument},
        {Error::NameTooLong, User::SyscallErrorNameTooLong},
        {Error::FileTooLarge, User::SyscallErrorFileTooLarge},
        {Error::TooManyLinks, User::SyscallErrorTooManyLinks},
        {Error::Busy, User::SyscallErrorBusy},
        {Error::NotPermitted, User::SyscallErrorNotPermitted},
        {Error::OutOfMemory, User::SyscallErrorNoMemory},
        {Error::Unsupported, User::SyscallErrorOperationNotSupported},
    };
    REQUIRE(expected.size() == static_cast<size_t>(Error::Unsupported) + 1);
    for (const auto& [error, code] : expected)
    {
        CAPTURE(static_cast<int>(error));
        REQUIRE(SyscallError(error) == code);
    }
}

TEST_CASE("FAT16 creates nested directories and files inside them")
{
    MakeFat16Disk("contents");
    REQUIRE(FeatherOS::VFS::MountBlockDevice());
    REQUIRE(FeatherOS::VFS::MakeDirectory("/docs"));
    REQUIRE(FeatherOS::VFS::MakeDirectory("/DOCS/old"));
    REQUIRE_FALSE(FeatherOS::VFS::MakeDirectory("/docs"));
    REQUIRE_FALSE(FeatherOS::VFS::MakeDirectory("/missing/sub"));
    WriteWhole("/docs/notes.txt", Pattern(700));
    WriteWhole("/docs/old/a.txt", "deep");

    REQUIRE(ListDiskRoot() == std::vector<std::string>{"BLOCK.TXT", "docs/"});
    REQUIRE(ListDirectory("/docs") == std::vector<std::string>{"old/", "notes.txt"});
    REQUIRE(ListDirectory("/docs/old/") == std::vector<std::string>{"a.txt"});
    REQUIRE(ReadWhole("/docs/notes.txt") == Pattern(700));
    REQUIRE(ReadWhole("/docs/old/../old/./a.txt") == "deep");
    REQUIRE(ReadWhole("/docs/old/../../BLOCK.TXT") == "contents");
    REQUIRE(ReadWhole("/../BLOCK.TXT") == "contents");

    // A directory is not a file, and a file is not a directory.
    FeatherOS::VFS::FileHandle file{};
    REQUIRE_FALSE(FeatherOS::VFS::Open("/docs", &file));
    REQUIRE_FALSE(FeatherOS::VFS::Remove("/docs"));
    REQUIRE_FALSE(FeatherOS::VFS::Open("/BLOCK.TXT/x", &file));
    REQUIRE_FALSE(FeatherOS::VFS::RemoveDirectory("/BLOCK.TXT"));
}

TEST_CASE("FAT16 grows a subdirectory past its first cluster")
{
    MakeFat16Disk("contents");
    REQUIRE(FeatherOS::VFS::MountBlockDevice());
    REQUIRE(FeatherOS::VFS::MakeDirectory("/many"));
    const uint64_t free_before = FreeDiskBlocks();
    // One-sector clusters hold 16 entries, and "." and ".." take two of them.
    std::vector<std::string> expected;
    for (int index = 0; index < 40; ++index)
    {
        const std::string name = "F" + std::to_string(index) + ".TXT";
        FeatherOS::VFS::FileHandle file{};
        REQUIRE(FeatherOS::VFS::Create(("/many/" + name).c_str(), &file));
        expected.push_back(name);
    }
    REQUIRE(FreeDiskBlocks() == free_before - 2);
    REQUIRE(ListDirectory("/many") == expected);
    REQUIRE(ReadWhole("/many/F39.TXT").empty());
}

TEST_CASE("FAT16 removes only empty directories and frees their clusters")
{
    MakeFat16Disk("contents");
    REQUIRE(FeatherOS::VFS::MountBlockDevice());
    const uint64_t free_before = FreeDiskBlocks();
    REQUIRE(FeatherOS::VFS::MakeDirectory("/scratch"));
    WriteWhole("/scratch/x.txt", "x");
    REQUIRE_FALSE(FeatherOS::VFS::RemoveDirectory("/scratch"));
    REQUIRE(FeatherOS::VFS::Remove("/scratch/x.txt"));
    REQUIRE(FeatherOS::VFS::RemoveDirectory("/scratch"));
    REQUIRE(FreeDiskBlocks() == free_before);
    REQUIRE(ListDiskRoot() == std::vector<std::string>{"BLOCK.TXT"});
    REQUIRE_FALSE(FeatherOS::VFS::RemoveDirectory("/scratch"));
    REQUIRE_FALSE(FeatherOS::VFS::RemoveDirectory("/"));
}

TEST_CASE("FAT16 renames files and directories, keeping their data")
{
    MakeFat16Disk("contents");
    REQUIRE(FeatherOS::VFS::MountBlockDevice());
    WriteWhole("/a.txt", Pattern(700));
    WriteWhole("/c.txt", "c");
    REQUIRE(FeatherOS::VFS::MakeDirectory("/docs"));
    REQUIRE(FeatherOS::VFS::MakeDirectory("/docs/old"));
    WriteWhole("/docs/old/x.txt", "deep");
    const uint64_t free_before = FreeDiskBlocks();

    REQUIRE(FeatherOS::VFS::Rename("/a.txt", "/b.txt"));
    REQUIRE(FeatherOS::VFS::Rename("/b.txt", "/A much longer name.txt"));
    REQUIRE(FeatherOS::VFS::Rename("/a much longer NAME.txt", "/a MUCH longer name.txt"));
    REQUIRE(FeatherOS::VFS::Rename("/c.txt", "/C.TXT"));
    REQUIRE(ReadWhole("/a MUCH longer name.txt") == Pattern(700));
    REQUIRE(Sorted(ListDiskRoot()) ==
            std::vector<std::string>{"BLOCK.TXT", "C.TXT", "a MUCH longer name.txt", "docs/"});

    // A moved directory's ".." follows it.
    REQUIRE(FeatherOS::VFS::Rename("/docs/old", "/old"));
    REQUIRE(ReadWhole("/old/../BLOCK.TXT") == "contents");
    REQUIRE(FeatherOS::VFS::Rename("/old", "/docs/archive"));
    REQUIRE(ReadWhole("/docs/archive/x.txt") == "deep");
    REQUIRE(ReadWhole("/docs/archive/../../BLOCK.TXT") == "contents");
    REQUIRE(ListDirectory("/docs") == std::vector<std::string>{"archive/"});
    REQUIRE(FreeDiskBlocks() == free_before);

    REQUIRE_FALSE(FeatherOS::VFS::Rename("/docs", "/docs/archive/inner"));
    REQUIRE_FALSE(FeatherOS::VFS::Rename("/docs", "/DOCS/inner"));
    REQUIRE_FALSE(FeatherOS::VFS::Rename("/C.TXT", "/missing/c.txt"));
    REQUIRE_FALSE(FeatherOS::VFS::Rename("/C.TXT", "/bad:name"));
    REQUIRE_FALSE(FeatherOS::VFS::Rename("/gone", "/here"));
    REQUIRE_FALSE(FeatherOS::VFS::Rename("/docs/..", "/up"));
    REQUIRE(ReadWhole("/C.TXT") == "c");
    REQUIRE(FeatherOS::VFS::Rename("/C.TXT", "/BLOCK.TXT"));
    REQUIRE(ReadWhole("/BLOCK.TXT") == "c");
}

TEST_CASE("rename replaces files and empty directories, and refuses open files and other filesystems")
{
    MakeFat16Disk("contents");
    REQUIRE(FeatherOS::VFS::MountBlockDevice());
    FeatherOS::Files::Table table{};
    FeatherOS::Files::OpenConsole(&table, nullptr);
    WriteWhole("/new.txt", "new");
    REQUIRE(FeatherOS::Files::MakeDirectory("/docs") == 0);
    REQUIRE(FeatherOS::Files::MakeDirectory("/other") == 0);

    REQUIRE(FeatherOS::Files::Rename("/new.txt", "/BLOCK.TXT") == 0);
    REQUIRE(ReadWhole("/BLOCK.TXT") == "new");
    REQUIRE(FeatherOS::Files::Rename("/BLOCK.TXT", "/BLOCK.TXT") == 0);
    REQUIRE(FeatherOS::Files::Rename("/BLOCK.TXT", "/block.txt") == 0);
    REQUIRE(Sorted(ListDiskRoot()) == std::vector<std::string>{"block.txt", "docs/", "other/"});

    REQUIRE(FeatherOS::Files::Rename("/gone", "/here") == FeatherOS::User::SyscallErrorNotFound);
    REQUIRE(FeatherOS::Files::Rename("/block.txt", "/tmp/block.txt") ==
            FeatherOS::User::SyscallErrorCrossDevice);
    REQUIRE(FeatherOS::Files::Rename("/block.txt", "/system/block.txt") ==
            FeatherOS::User::SyscallErrorReadOnly);
    REQUIRE(FeatherOS::Files::Rename("/tmp", "/temporary") == FeatherOS::User::SyscallErrorBusy);
    REQUIRE(FeatherOS::Files::Rename("/docs", "/docs/sub") ==
            FeatherOS::User::SyscallErrorInvalidArgument);
    REQUIRE(FeatherOS::Files::Rename("/docs", "/block.txt") ==
            FeatherOS::User::SyscallErrorNotDirectory);
    REQUIRE(FeatherOS::Files::Rename("/block.txt", "/docs") ==
            FeatherOS::User::SyscallErrorIsDirectory);
    REQUIRE(FeatherOS::Files::Rename("/docs", "/other") == 0);
    REQUIRE(Sorted(ListDiskRoot()) == std::vector<std::string>{"block.txt", "other/"});
    REQUIRE(FeatherOS::Files::MakeDirectory("/docs") == 0);
    REQUIRE(FeatherOS::Files::Rename("/block.txt", "/missing/block.txt") ==
            FeatherOS::User::SyscallErrorNotFound);

    const uint64_t reader = FeatherOS::Files::Open(&table, "/block.txt", OpenRead);
    REQUIRE(FeatherOS::Files::Rename("/block.txt", "/docs/block.txt") ==
            FeatherOS::User::SyscallErrorBusy);
    REQUIRE(FeatherOS::Files::Close(&table, reader) == 0);
    REQUIRE(FeatherOS::Files::Rename("/block.txt", "/docs/block.txt") == 0);
    REQUIRE(FeatherOS::Files::Rename("/docs", "/other/docs") == 0);
    REQUIRE(ReadWhole("/other/docs/block.txt") == "new");
}

TEST_CASE("open-file sharing rules tell apart files with the same slot in different directories")
{
    MakeFat16Disk("contents");
    REQUIRE(FeatherOS::VFS::MountBlockDevice());
    REQUIRE(FeatherOS::VFS::MakeDirectory("/a"));
    REQUIRE(FeatherOS::VFS::MakeDirectory("/b"));
    WriteWhole("/a/same.txt", "a");
    WriteWhole("/b/same.txt", "b");
    FeatherOS::Files::Table table{};
    FeatherOS::Files::OpenConsole(&table, nullptr);
    const uint64_t first = FeatherOS::Files::Open(&table, "/a/same.txt", OpenWrite);
    const uint64_t second = FeatherOS::Files::Open(&table, "/b/same.txt", OpenWrite);
    REQUIRE_FALSE(FeatherOS::User::IsSyscallError(first));
    REQUIRE_FALSE(FeatherOS::User::IsSyscallError(second));
    REQUIRE(FeatherOS::Files::RemoveDirectory("/a") == FeatherOS::User::SyscallErrorNotEmpty);
    FeatherOS::Files::CloseAll(&table);
    FeatherOS::User::DirectoryEntry entry{};
    REQUIRE(FeatherOS::Files::ReadDirectory("/", 6, &entry) == 0);
    REQUIRE(std::string(entry.name) == "a");
    REQUIRE(entry.type == FeatherOS::User::ModeDirectory >> FeatherOS::User::EntryTypeShift);
}

TEST_CASE("FAT16 stores long names with numbered short aliases")
{
    MakeFat16Disk("contents");
    REQUIRE(FeatherOS::VFS::MountBlockDevice());
    WriteWhole("/My Notes.txt", "first");
    WriteWhole("/My Notes2.txt", "second");
    const uint8_t* root = disk[RootDirectorySector].data();
    // Slot 1 holds the long-name entry and slot 2 the short alias.
    REQUIRE(root[32 + 11] == 0x0F);
    REQUIRE(root[32] == 0x41);
    REQUIRE(std::memcmp(root + 64, "MYNOTE~1TXT", 11) == 0);
    REQUIRE(std::memcmp(root + 128, "MYNOTE~2TXT", 11) == 0);

    REQUIRE(ListDiskRoot() ==
            std::vector<std::string>{"BLOCK.TXT", "My Notes.txt", "My Notes2.txt"});
    REQUIRE(ReadWhole("/my notes.txt") == "first");
    REQUIRE(ReadWhole("/MYNOTE~1.TXT") == "first");
    REQUIRE(ReadWhole("/MY NOTES2.TXT") == "second");
    FeatherOS::VFS::FileHandle file{};
    REQUIRE_FALSE(FeatherOS::VFS::Create("/mynote~1.txt", &file));
    REQUIRE_FALSE(FeatherOS::VFS::Create("/MY NOTES.TXT", &file));
}

TEST_CASE("FAT16 keeps lower-case 8.3 names as short entries")
{
    MakeFat16Disk("contents");
    REQUIRE(FeatherOS::VFS::MountBlockDevice());
    WriteWhole("/readme.md", "text");
    const uint8_t* root = disk[RootDirectorySector].data();
    REQUIRE(std::memcmp(root + 32, "README  MD ", 11) == 0);
    REQUIRE(root[32 + 12] == 0x18);
    REQUIRE(ListDiskRoot() == std::vector<std::string>{"BLOCK.TXT", "readme.md"});
    REQUIRE(ReadWhole("/README.MD") == "text");
}

TEST_CASE("FAT16 removes a long-named file's name entries")
{
    MakeFat16Disk("contents");
    REQUIRE(FeatherOS::VFS::MountBlockDevice());
    WriteWhole("/a much longer file name.text", "x");
    REQUIRE(FeatherOS::VFS::Remove("/A MUCH LONGER FILE NAME.TEXT"));
    const uint8_t* root = disk[RootDirectorySector].data();
    for (uint32_t slot = 1; slot <= 4; ++slot)
    {
        REQUIRE(root[slot * 32] == 0xE5);
    }
    REQUIRE(ListDiskRoot() == std::vector<std::string>{"BLOCK.TXT"});
    WriteWhole("/another long name.txt", "y");
    REQUIRE(ListDiskRoot() ==
            std::vector<std::string>{"BLOCK.TXT", "another long name.txt"});
}

TEST_CASE("FAT16 long-named directories grow to fit a name's entries")
{
    MakeFat16Disk("contents");
    REQUIRE(FeatherOS::VFS::MountBlockDevice());
    REQUIRE(FeatherOS::VFS::MakeDirectory("/My Documents"));
    // "." and ".." plus ten short files leave four free slots in the first cluster, one too few
    // for a name needing four long-name entries and its short entry.
    for (int index = 0; index < 10; ++index)
    {
        WriteWhole(("/my documents/f" + std::to_string(index)).c_str(), "");
    }
    const std::string long_name = "a name that needs four long entries.txt";
    WriteWhole(("/My Documents/" + long_name).c_str(), "deep");
    REQUIRE(ReadWhole(("/my documents/" + long_name).c_str()) == "deep");
    REQUIRE(ListDirectory("/My Documents").back() == long_name);
    REQUIRE(ListDiskRoot() == std::vector<std::string>{"BLOCK.TXT", "My Documents/"});
    REQUIRE_FALSE(FeatherOS::VFS::RemoveDirectory("/my documents"));
}

TEST_CASE("the VFS root is the disk, with the initramfs and virtual filesystems mounted in it")
{
    MakeFat16Disk("contents");
    REQUIRE(FeatherOS::VFS::MountBlockDevice());
    const std::vector<uint8_t> archive = MakeTarArchive("system/hello.txt", "archive");
    REQUIRE(FeatherOS::Initramfs::Mount(archive.data(), archive.size()));
    REQUIRE(FeatherOS::VFS::IsDirectory("/"));
    REQUIRE(ListDirectory("/") ==
            std::vector<std::string>{"system/", "etc/", "dev/", "proc/", "tmp/", "BLOCK.TXT"});
    REQUIRE(ListDirectory("/system") == std::vector<std::string>{"hello.txt"});
    REQUIRE(ReadWhole("/system/hello.txt") == "archive");
    FeatherOS::Files::Table table{};
    FeatherOS::Files::OpenConsole(&table, nullptr);
    REQUIRE(FeatherOS::Files::CheckDirectory("/") == 0);
    REQUIRE(FeatherOS::Files::CheckDirectory("/system") == 0);
    REQUIRE(FeatherOS::Files::CheckDirectory("/system/hello.txt") ==
            FeatherOS::User::SyscallErrorNotDirectory);
    REQUIRE(FeatherOS::Files::CheckDirectory("/system/missing") ==
            FeatherOS::User::SyscallErrorNotFound);
    REQUIRE(FeatherOS::Files::Open(&table, "/system/hello.txt", OpenWrite) ==
            FeatherOS::User::SyscallErrorReadOnly);
}

TEST_CASE("file syscalls report why a change failed")
{
    MakeFat16Disk("contents");
    REQUIRE(FeatherOS::VFS::MountBlockDevice());
    const std::vector<uint8_t> archive = MakeTarArchive("system/hello.txt", "archive");
    REQUIRE(FeatherOS::Initramfs::Mount(archive.data(), archive.size()));
    FeatherOS::Files::Table table{};
    FeatherOS::Files::OpenConsole(&table, nullptr);
    REQUIRE(FeatherOS::Files::MakeDirectory("/docs") == 0);
    REQUIRE(FeatherOS::Files::MakeDirectory("/docs") == FeatherOS::User::SyscallErrorExists);
    REQUIRE(FeatherOS::Files::MakeDirectory("/BLOCK.TXT") == FeatherOS::User::SyscallErrorExists);
    REQUIRE(FeatherOS::Files::MakeDirectory("/missing/docs") ==
            FeatherOS::User::SyscallErrorNotFound);
    REQUIRE(FeatherOS::Files::MakeDirectory("/system/docs") ==
            FeatherOS::User::SyscallErrorReadOnly);
    REQUIRE(FeatherOS::Files::Open(&table, "/missing/new.txt", OpenWrite | OpenCreate) ==
            FeatherOS::User::SyscallErrorNotFound);
    REQUIRE(FeatherOS::Files::Open(&table, "/system/new.txt", OpenWrite | OpenCreate) ==
            FeatherOS::User::SyscallErrorReadOnly);
    REQUIRE(FeatherOS::Files::Open(&table, "/docs", OpenRead) ==
            FeatherOS::User::SyscallErrorIsDirectory);
    REQUIRE(FeatherOS::Files::Remove("/docs") == FeatherOS::User::SyscallErrorIsDirectory);
    REQUIRE(FeatherOS::Files::RemoveDirectory("/BLOCK.TXT") ==
            FeatherOS::User::SyscallErrorNotDirectory);
    REQUIRE(FeatherOS::Files::RemoveDirectory("/gone") == FeatherOS::User::SyscallErrorNotFound);
    REQUIRE(FeatherOS::Files::RemoveDirectory("/system") == FeatherOS::User::SyscallErrorReadOnly);
    REQUIRE(FeatherOS::Files::RemoveDirectory("/") == FeatherOS::User::SyscallErrorBusy);
    REQUIRE(FeatherOS::Files::RemoveDirectory("/docs/.") ==
            FeatherOS::User::SyscallErrorInvalidArgument);
    REQUIRE(FeatherOS::Files::RemoveDirectory("/docs/..") == FeatherOS::User::SyscallErrorNotEmpty);
    REQUIRE(FeatherOS::Files::Rename("/docs/.", "/other") == FeatherOS::User::SyscallErrorBusy);
    REQUIRE(FeatherOS::Files::RemoveDirectory("/docs") == 0);
    REQUIRE(FeatherOS::Files::Seek(&table, 9, 0, FeatherOS::User::SeekFromStart) ==
            FeatherOS::User::SyscallErrorBadDescriptor);
}

TEST_CASE("stat reports type, size, and whether a file is read-only")
{
    MakeFat16Disk("contents");
    REQUIRE(FeatherOS::VFS::MountBlockDevice());
    const std::vector<uint8_t> archive = MakeTarArchive("system/hello.txt", "archive");
    REQUIRE(FeatherOS::Initramfs::Mount(archive.data(), archive.size()));
    namespace User = FeatherOS::User;
    FeatherOS::User::FileStatus status{};
    REQUIRE(FeatherOS::Files::Stat("/BLOCK.TXT", &status) == 0);
    REQUIRE(status.type == FeatherOS::User::FileType::Regular);
    REQUIRE(status.size == 8);
    REQUIRE_FALSE(status.read_only);
    // FAT16 through the core: fixed modes, one link, no owners and, for now, no times.
    REQUIRE(status.mode == (User::ModeRegular | 0644));
    REQUIRE(status.link_count == 1);
    REQUIRE(status.owner == 0);
    REQUIRE(status.modify_time.seconds == 0);
    const uint64_t block_node = status.node;
    const uint64_t disk_device = status.device;
    User::DirectoryEntry entry{};
    REQUIRE(FeatherOS::Files::ReadDirectory("/", 5, &entry) == 0);
    REQUIRE(std::string(entry.name) == "BLOCK.TXT");
    REQUIRE(entry.node == block_node);
    REQUIRE(entry.type == User::ModeRegular >> User::EntryTypeShift);
    REQUIRE(FeatherOS::Files::Stat("/system/hello.txt", &status) == 0);
    REQUIRE(status.type == FeatherOS::User::FileType::Regular);
    REQUIRE(status.size == 7);
    REQUIRE(status.read_only);
    REQUIRE(status.mode == (User::ModeRegular | 0444));
    REQUIRE(status.device != disk_device);
    REQUIRE(FeatherOS::Files::Stat("/", &status) == 0);
    REQUIRE(status.type == FeatherOS::User::FileType::Directory);
    REQUIRE_FALSE(status.read_only);
    REQUIRE(status.mode == (User::ModeDirectory | 0755));
    REQUIRE(status.device == disk_device);
    REQUIRE(FeatherOS::Files::Stat("/system", &status) == 0);
    REQUIRE(status.type == FeatherOS::User::FileType::Directory);
    REQUIRE(status.read_only);
    REQUIRE(status.mode == (User::ModeDirectory | 0555));
    REQUIRE(FeatherOS::Files::Stat("/missing", &status) == FeatherOS::User::SyscallErrorNotFound);

    FeatherOS::Files::Table table{};
    FeatherOS::Files::OpenConsole(&table, nullptr);
    const uint64_t writer = FeatherOS::Files::Open(&table, "/BLOCK.TXT", OpenWrite | OpenAppend);
    REQUIRE(FeatherOS::Files::Write(&table, writer, "++", 2) == 2);
    REQUIRE(FeatherOS::Files::StatDescriptor(&table, writer, &status) == 0);
    REQUIRE(status.size == 10);
    REQUIRE_FALSE(status.read_only);
    REQUIRE(status.node == block_node);
    REQUIRE(status.device == disk_device);
    REQUIRE(status.mode == (User::ModeRegular | 0644));
    REQUIRE(FeatherOS::Files::StatDescriptor(&table, writer + 1, &status) ==
            FeatherOS::User::SyscallErrorBadDescriptor);
    FeatherOS::Files::CloseAll(&table);
}

TEST_CASE("dup shares the open file and its offset until the last descriptor closes")
{
    MakeFat16Disk("contents");
    REQUIRE(FeatherOS::VFS::MountBlockDevice());
    FeatherOS::Files::Table table{};
    FeatherOS::Files::OpenConsole(&table, nullptr);
    const uint64_t original = FeatherOS::Files::Open(&table, "/BLOCK.TXT", OpenRead | OpenWrite);
    const uint64_t copy = FeatherOS::Files::Duplicate(&table, original, 0, 0);
    REQUIRE(copy == original + 1);
    char text[8] = {};
    REQUIRE(FeatherOS::Files::Read(&table, original, text, 3) == 3);
    REQUIRE(FeatherOS::Files::Read(&table, copy, text + 3, 5) == 5);
    REQUIRE(std::string(text, 8) == "contents");
    REQUIRE(FeatherOS::Files::Close(&table, original) == 0);
    REQUIRE(FeatherOS::Files::Remove("/BLOCK.TXT") == FeatherOS::User::SyscallErrorBusy);
    REQUIRE(FeatherOS::Files::Seek(&table, copy, 0, FeatherOS::User::SeekFromStart) == 0);
    REQUIRE(FeatherOS::Files::Close(&table, copy) == 0);
    FeatherOS::Files::Table other{};
    FeatherOS::Files::OpenConsole(&other, nullptr);
    const uint64_t writer = FeatherOS::Files::Open(&other, "/BLOCK.TXT", OpenWrite);
    REQUIRE_FALSE(FeatherOS::User::IsSyscallError(writer));
    FeatherOS::Files::CloseAll(&other);
    FeatherOS::Files::CloseAll(&table);
}

TEST_CASE("dup2 replaces its target, and duplicates can be close-on-exec")
{
    MakeFat16Disk("contents");
    REQUIRE(FeatherOS::VFS::MountBlockDevice());
    FeatherOS::Files::Table table{};
    FeatherOS::Files::OpenConsole(&table, nullptr);
    const uint64_t file = FeatherOS::Files::Open(&table, "/BLOCK.TXT", OpenRead);
    REQUIRE(FeatherOS::Files::KindOf(&table, 1) == FeatherOS::Files::Kind::Terminal);
    REQUIRE(FeatherOS::Files::Duplicate(&table, file, 1, FeatherOS::User::DuplicateExact) == 1);
    REQUIRE(FeatherOS::Files::KindOf(&table, 1) == FeatherOS::Files::Kind::File);
    REQUIRE(FeatherOS::Files::Duplicate(&table, 1, 1, FeatherOS::User::DuplicateExact) == 1);
    REQUIRE(FeatherOS::Files::Duplicate(&table, file, 10,
                                        FeatherOS::User::DuplicateCloseOnExec) == 10);
    REQUIRE(FeatherOS::Files::DescriptorFlags(&table, 10, false, 0) ==
            FeatherOS::User::DescriptorCloseOnExec);
    REQUIRE(FeatherOS::Files::DescriptorFlags(&table, 10, true, 0) == 0);
    REQUIRE(FeatherOS::Files::DescriptorFlags(&table, file, true, 4) ==
            FeatherOS::User::SyscallErrorInvalidArgument);
    REQUIRE(FeatherOS::Files::Duplicate(&table, 20, 0, 0) ==
            FeatherOS::User::SyscallErrorBadDescriptor);
    REQUIRE(FeatherOS::Files::Duplicate(&table, file, FeatherOS::User::DefaultDescriptorLimit,
                                        FeatherOS::User::DuplicateExact) ==
            FeatherOS::User::SyscallErrorBadDescriptor);
    REQUIRE(FeatherOS::Files::Duplicate(&table, file, 0, 8) ==
            FeatherOS::User::SyscallErrorInvalidArgument);
    REQUIRE(FeatherOS::Files::CountOpen(&table) == 5);
    REQUIRE(FeatherOS::Files::Close(&table, 1) == 0);
    REQUIRE(FeatherOS::Files::KindOf(&table, 1) == FeatherOS::Files::Kind::None);
    REQUIRE(FeatherOS::Files::Seek(&table, 0, 0, FeatherOS::User::SeekFromStart) ==
            FeatherOS::User::SyscallErrorNotSeekable);
    FeatherOS::Files::CloseAll(&table);
}

TEST_CASE("children inherit descriptors that are not close-on-exec, sharing their offsets")
{
    MakeFat16Disk("contents");
    REQUIRE(FeatherOS::VFS::MountBlockDevice());
    FeatherOS::Files::Table parent{};
    FeatherOS::Files::OpenConsole(&parent, nullptr);
    const uint64_t shared = FeatherOS::Files::Open(&parent, "/BLOCK.TXT", OpenRead);
    const uint64_t private_file =
        FeatherOS::Files::Open(&parent, "/BLOCK.TXT", OpenRead | FeatherOS::User::OpenCloseOnExec);
    REQUIRE(FeatherOS::Files::Duplicate(&parent, shared, 0, FeatherOS::User::DuplicateExact) ==
            0);
    FeatherOS::Files::Table child{};
    FeatherOS::Files::Inherit(&child, &parent);
    REQUIRE(FeatherOS::Files::KindOf(&child, 0) == FeatherOS::Files::Kind::File);
    REQUIRE(FeatherOS::Files::KindOf(&child, 1) == FeatherOS::Files::Kind::Terminal);
    REQUIRE(FeatherOS::Files::KindOf(&child, shared) == FeatherOS::Files::Kind::File);
    REQUIRE(FeatherOS::Files::KindOf(&child, private_file) == FeatherOS::Files::Kind::None);
    char text[4] = {};
    REQUIRE(FeatherOS::Files::Read(&child, 0, text, 4) == 4);
    REQUIRE(FeatherOS::Files::Read(&parent, shared, text, 4) == 4);
    REQUIRE(std::string(text, 4) == "ents");
    FeatherOS::Files::CloseAll(&parent);
    REQUIRE(FeatherOS::Files::Read(&child, shared, text, 4) == 0);
    FeatherOS::Files::CloseAll(&child);
    REQUIRE(FeatherOS::Files::Remove("/BLOCK.TXT") == 0);
}

TEST_CASE("append writes land at the end through every descriptor")
{
    MakeFat16Disk("contents");
    REQUIRE(FeatherOS::VFS::MountBlockDevice());
    FeatherOS::Files::Table table{};
    FeatherOS::Files::OpenConsole(&table, nullptr);
    const uint64_t appender = FeatherOS::Files::Open(&table, "/BLOCK.TXT", OpenWrite | OpenAppend);
    const uint64_t copy = FeatherOS::Files::Duplicate(&table, appender, 0, 0);
    REQUIRE(FeatherOS::Files::Seek(&table, appender, 0, FeatherOS::User::SeekFromStart) == 0);
    REQUIRE(FeatherOS::Files::Write(&table, appender, "+", 1) == 1);
    REQUIRE(FeatherOS::Files::Write(&table, copy, "-", 1) == 1);
    FeatherOS::Files::CloseAll(&table);
    REQUIRE(ReadWhole("/BLOCK.TXT") == "contents+-");
}

TEST_CASE("unmounting the disk flushes it, and mounting it again shows the same files")
{
    using FeatherOS::VFS::MountResult;
    MakeFat16Disk("contents");
    REQUIRE(FeatherOS::VFS::MountBlockDevice());
    FeatherOS::VFS::FileHandle file{};
    REQUIRE(FeatherOS::VFS::Create("/kept.txt", &file));
    uint64_t written = 0;
    REQUIRE(FeatherOS::VFS::Write(&file, "kept", 4, &written));
    FeatherOS::VFS::Close(&file);

    REQUIRE(FeatherOS::VFS::Unmount("/") == MountResult::Busy);
    constexpr const char* InnerPoints[] = {"/system", "/etc", "/dev", "/proc", "/tmp"};
    for (const char* point : InnerPoints)
    {
        REQUIRE(FeatherOS::VFS::Unmount(point) == MountResult::Done);
    }
    REQUIRE(FeatherOS::VFS::Open("/kept.txt", &file));
    REQUIRE(FeatherOS::VFS::Unmount("/") == MountResult::Busy);
    FeatherOS::VFS::Close(&file);
    flush_count = 0;
    REQUIRE(FeatherOS::VFS::Unmount("/") == MountResult::Done);
    REQUIRE(flush_count == 1);
    REQUIRE_FALSE(FeatherOS::VFS::Open("/kept.txt", &file));
    REQUIRE(FeatherOS::VFS::Unmount("/") == MountResult::NotFound);

    REQUIRE(FeatherOS::VFS::MountBuiltIn());
    REQUIRE(FeatherOS::VFS::Mount("fat16", "sda", "/") == MountResult::NotFound);
    REQUIRE(FeatherOS::VFS::Mount("fat16", "vda", "/") == MountResult::NotFound);
    REQUIRE(FeatherOS::VFS::Mount("ext2", "vda1", "/") == MountResult::UnknownType);
    REQUIRE(FeatherOS::VFS::Mount("fat16", "vda1", "/") == MountResult::Done);
    REQUIRE(FeatherOS::VFS::Mount("fat16", "vda1", "/mnt") == MountResult::Busy);
    REQUIRE(ReadWhole("/kept.txt") == "kept");
}

TEST_CASE("the periodic sync leaves FAT16 alone")
{
    MakeFat16Disk("contents");
    REQUIRE(FeatherOS::VFS::MountBlockDevice());
    REQUIRE(MountSource("/") == "vda1 fat16 rw");
    REQUIRE(MountSource("/mnt").empty());
    WriteAndClose("/periodic.txt", "fat\n");
    const uint32_t flushes = flush_count;
    REQUIRE(FeatherOS::Files::SyncPeriodic() == 0);
    REQUIRE(flush_count == flushes);
}
