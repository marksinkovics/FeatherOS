#include <catch2/catch_test_macros.hpp>

#include "kernel/files.h"
#include "kernel/filesystem/fat16.h"
#include "kernel/filesystem/fat_names.h"
#include "kernel/filesystem/initramfs.h"
#include "kernel/filesystem/vfs.h"
#include "kernel/storage/block_device.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace
{

constexpr uint32_t DiskSectorCount = 5001;
constexpr uint32_t PartitionStart = 1;
constexpr uint32_t PartitionSectorCount = 5000;
constexpr uint32_t FatSizeSectors = 20;
constexpr uint32_t RootDirectorySectors = 1;
constexpr uint32_t RootDirectorySector = PartitionStart + 1 + FatSizeSectors;
constexpr uint32_t DataStartSector = RootDirectorySector + RootDirectorySectors;

std::vector<std::array<uint8_t, FeatherOS::BlockDevice::SectorSize>> disk;

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

void MakeFat16Disk(const std::string& contents)
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
    WriteLittleEndian16(boot + 17, 16);
    WriteLittleEndian16(boot + 19, PartitionSectorCount);
    boot[21] = 0xF8;
    WriteLittleEndian16(boot + 22, FatSizeSectors);
    boot[510] = 0x55;
    boot[511] = 0xAA;

    AddRootFile(contents);
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

std::vector<uint8_t> MakeTarArchive(const std::string& path, const std::string& contents)
{
    std::vector<uint8_t> archive(3 * 512, 0);
    auto* header = reinterpret_cast<char*>(archive.data());
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
    std::memcpy(archive.data() + 512, contents.data(), contents.size());
    return archive;
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
    if (buffer == nullptr || sector >= disk.size())
    {
        return false;
    }
    std::memcpy(buffer, disk[sector].data(), SectorSize);
    return true;
}

bool WriteSector(uint64_t sector, const void* buffer)
{
    if (buffer == nullptr || sector >= disk.size())
    {
        return false;
    }
    std::memcpy(disk[sector].data(), buffer, SectorSize);
    return true;
}

} // namespace FeatherOS::BlockDevice

TEST_CASE("USTAR initramfs exposes file contents")
{
    const std::string contents = "FeatherOS unit test archive";
    const std::vector<uint8_t> archive = MakeTarArchive("hello.txt", contents);
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

TEST_CASE("USTAR initramfs rejects a corrupt header checksum")
{
    std::vector<uint8_t> archive = MakeTarArchive("hello.txt", "contents");
    archive[0] ^= 1;
    REQUIRE(FeatherOS::Initramfs::Mount(archive.data(), archive.size()));
    FeatherOS::Initramfs::File file{};
    REQUIRE_FALSE(FeatherOS::Initramfs::Open("hello.txt", &file));
}

TEST_CASE("USTAR initramfs rejects a truncated file payload")
{
    std::vector<uint8_t> archive = MakeTarArchive("hello.txt", std::string(700, 'x'));
    archive.resize(2 * 512);
    REQUIRE(FeatherOS::Initramfs::Mount(archive.data(), archive.size()));
    FeatherOS::Initramfs::File file{};
    REQUIRE_FALSE(FeatherOS::Initramfs::Open("hello.txt", &file));
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
    REQUIRE_FALSE(FeatherOS::FAT16::Mount());
}

TEST_CASE("FAT16 mount rejects unsupported bytes per sector")
{
    MakeFat16Disk("contents");
    WriteLittleEndian16(disk[PartitionStart].data() + 11, 1024);
    REQUIRE_FALSE(FeatherOS::FAT16::Mount());
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
    return contents;
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
    const uint32_t free_before = FeatherOS::FAT16::FreeClusterCount();

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
    REQUIRE(FeatherOS::FAT16::FreeClusterCount() == free_before - 3);
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
    const uint32_t free_full = FeatherOS::FAT16::FreeClusterCount();

    REQUIRE(FeatherOS::VFS::Truncate(&file, 600));
    REQUIRE(FeatherOS::FAT16::FreeClusterCount() == free_full + 2);
    REQUIRE(ReadWhole("/SHRINK.TXT") == contents.substr(0, 600));
    REQUIRE_FALSE(FeatherOS::VFS::Truncate(&file, 601));

    REQUIRE(FeatherOS::VFS::Truncate(&file, 0));
    REQUIRE(FeatherOS::FAT16::FreeClusterCount() == free_full + 4);
    REQUIRE(file.first_cluster == 0);
    REQUIRE(ReadWhole("/SHRINK.TXT").empty());
}

TEST_CASE("FAT16 removal frees the chain and the directory slot")
{
    MakeFat16Disk(std::string(700, 'x'));
    REQUIRE(FeatherOS::VFS::MountBlockDevice());
    const uint32_t free_before = FeatherOS::FAT16::FreeClusterCount();
    REQUIRE(FeatherOS::VFS::Remove("/BLOCK.TXT"));
    FeatherOS::VFS::FileInfo info{};
    REQUIRE_FALSE(FeatherOS::VFS::Stat("/BLOCK.TXT", &info));
    REQUIRE(FeatherOS::FAT16::FreeClusterCount() == free_before + 2);
    REQUIRE(FatEntry(2) == 0);
    REQUIRE(FatEntry(3) == 0);
    REQUIRE(disk[RootDirectorySector][0] == 0xE5);
    REQUIRE_FALSE(FeatherOS::VFS::Remove("/BLOCK.TXT"));

    FeatherOS::VFS::FileHandle file{};
    REQUIRE(FeatherOS::VFS::Create("/AGAIN.TXT", &file));
    REQUIRE(file.entry_index == 0);
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
    const uint32_t free_clusters = FeatherOS::FAT16::FreeClusterCount();
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
    REQUIRE(FeatherOS::FAT16::FreeClusterCount() == 1);

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

TEST_CASE("file descriptors start at 3 and are limited per process")
{
    MakeFat16Disk("contents");
    REQUIRE(FeatherOS::VFS::MountBlockDevice());
    FeatherOS::Files::Table table{};
    std::vector<uint64_t> descriptors;
    for (uint64_t index = 0; index < FeatherOS::User::MaximumOpenFiles; ++index)
    {
        const uint64_t descriptor = FeatherOS::Files::Open(&table, "/BLOCK.TXT", OpenRead);
        REQUIRE(descriptor == FeatherOS::User::FirstFileDescriptor + index);
        descriptors.push_back(descriptor);
    }
    REQUIRE(FeatherOS::Files::Open(&table, "/BLOCK.TXT", OpenRead) ==
            FeatherOS::User::SyscallErrorTooManyOpenFiles);
    REQUIRE(FeatherOS::Files::Close(&table, descriptors[4]) == 0);
    REQUIRE(FeatherOS::Files::Open(&table, "/BLOCK.TXT", OpenRead) == descriptors[4]);
    FeatherOS::Files::CloseAll(&table);
    REQUIRE(FeatherOS::Files::Close(&table, descriptors[0]) ==
            FeatherOS::User::SyscallErrorBadDescriptor);
    REQUIRE(FeatherOS::Files::Close(&table, 0) == FeatherOS::User::SyscallErrorBadDescriptor);
}

TEST_CASE("file descriptors validate flags and access modes")
{
    MakeFat16Disk("contents");
    REQUIRE(FeatherOS::VFS::MountBlockDevice());
    const std::vector<uint8_t> archive = MakeTarArchive("hello.txt", "archive");
    REQUIRE(FeatherOS::Initramfs::Mount(archive.data(), archive.size()));
    FeatherOS::Files::Table table{};
    REQUIRE(FeatherOS::Files::Open(&table, "/BLOCK.TXT", 0) ==
            FeatherOS::User::SyscallErrorInvalidArgument);
    REQUIRE(FeatherOS::Files::Open(&table, "/BLOCK.TXT", OpenRead | 64) ==
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
    FeatherOS::Files::Table second{};
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

TEST_CASE("directory listing skips deleted entries and names files as NAME.EXT")
{
    MakeFat16Disk("contents");
    REQUIRE(FeatherOS::VFS::MountBlockDevice());
    FeatherOS::VFS::FileHandle file{};
    REQUIRE(FeatherOS::VFS::Create("/GONE.TXT", &file));
    REQUIRE(FeatherOS::VFS::Create("/README", &file));
    REQUIRE(FeatherOS::VFS::Remove("/GONE.TXT"));

    FeatherOS::Files::Table table{};
    const uint64_t writer =
        FeatherOS::Files::Open(&table, "/README", OpenWrite | OpenAppend);
    REQUIRE(FeatherOS::Files::Write(&table, writer, "abc", 3) == 3);
    REQUIRE(FeatherOS::Files::Close(&table, writer) == 0);

    FeatherOS::User::DirectoryEntry entry{};
    // Indices 0-3 of the root are the mount points.
    REQUIRE(FeatherOS::Files::ReadDirectory("/", 0, &entry) == 0);
    REQUIRE(std::string(entry.name) == "system");
    REQUIRE(entry.directory);
    REQUIRE(FeatherOS::Files::ReadDirectory("/", 4, &entry) == 0);
    REQUIRE(std::string(entry.name) == "BLOCK.TXT");
    REQUIRE(entry.size == 8);
    REQUIRE(FeatherOS::Files::ReadDirectory("/", 5, &entry) == 0);
    REQUIRE(std::string(entry.name) == "README");
    REQUIRE(entry.size == 3);
    REQUIRE(FeatherOS::Files::ReadDirectory("/", 6, &entry) ==
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

TEST_CASE("a GUID partition table with a bad header or entry CRC mounts nothing")
{
    MakeFat16Disk("contents");
    ConvertToGpt();
    disk[1][40] ^= 1;
    REQUIRE_FALSE(FeatherOS::VFS::MountBlockDevice());

    MakeFat16Disk("contents");
    ConvertToGpt();
    disk[2][100] ^= 1;
    REQUIRE_FALSE(FeatherOS::VFS::MountBlockDevice());

    MakeFat16Disk("contents");
    ConvertToGpt();
    std::memcpy(disk[1].data(), "EFI PARX", 8);
    REQUIRE_FALSE(FeatherOS::VFS::MountBlockDevice());
}

namespace
{

std::vector<std::string> ListDirectory(const char* path)
{
    std::vector<std::string> names;
    FeatherOS::FAT16::DirectoryEntry entry{};
    for (uint32_t index = 0; FeatherOS::VFS::ReadDirectory(path, index, &entry); ++index)
    {
        names.push_back(std::string(entry.name) + (entry.directory ? "/" : ""));
    }
    return names;
}

// The disk's root entries, without the mount points listed first.
std::vector<std::string> ListDiskRoot()
{
    std::vector<std::string> names = ListDirectory("/");
    const std::vector<std::string> mounts = {"system/", "dev/", "proc/", "tmp/"};
    REQUIRE(names.size() >= mounts.size());
    REQUIRE(std::vector<std::string>(names.begin(), names.begin() + 4) == mounts);
    names.erase(names.begin(), names.begin() + 4);
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

} // namespace

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
    const uint32_t free_before = FeatherOS::FAT16::FreeClusterCount();
    // One-sector clusters hold 16 entries, and "." and ".." take two of them.
    std::vector<std::string> expected;
    for (int index = 0; index < 40; ++index)
    {
        const std::string name = "F" + std::to_string(index) + ".TXT";
        FeatherOS::VFS::FileHandle file{};
        REQUIRE(FeatherOS::VFS::Create(("/many/" + name).c_str(), &file));
        expected.push_back(name);
    }
    REQUIRE(FeatherOS::FAT16::FreeClusterCount() == free_before - 2);
    REQUIRE(ListDirectory("/many") == expected);
    REQUIRE(ReadWhole("/many/F39.TXT").empty());
}

TEST_CASE("FAT16 removes only empty directories and frees their clusters")
{
    MakeFat16Disk("contents");
    REQUIRE(FeatherOS::VFS::MountBlockDevice());
    const uint32_t free_before = FeatherOS::FAT16::FreeClusterCount();
    REQUIRE(FeatherOS::VFS::MakeDirectory("/scratch"));
    WriteWhole("/scratch/x.txt", "x");
    REQUIRE_FALSE(FeatherOS::VFS::RemoveDirectory("/scratch"));
    REQUIRE(FeatherOS::VFS::Remove("/scratch/x.txt"));
    REQUIRE(FeatherOS::VFS::RemoveDirectory("/scratch"));
    REQUIRE(FeatherOS::FAT16::FreeClusterCount() == free_before);
    REQUIRE(ListDiskRoot() == std::vector<std::string>{"BLOCK.TXT"});
    REQUIRE_FALSE(FeatherOS::VFS::RemoveDirectory("/scratch"));
    REQUIRE_FALSE(FeatherOS::VFS::RemoveDirectory("/"));
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
    const uint64_t first = FeatherOS::Files::Open(&table, "/a/same.txt", OpenWrite);
    const uint64_t second = FeatherOS::Files::Open(&table, "/b/same.txt", OpenWrite);
    REQUIRE_FALSE(FeatherOS::User::IsSyscallError(first));
    REQUIRE_FALSE(FeatherOS::User::IsSyscallError(second));
    REQUIRE(FeatherOS::Files::RemoveDirectory("/a") == FeatherOS::User::SyscallErrorNotEmpty);
    FeatherOS::Files::CloseAll(&table);
    FeatherOS::User::DirectoryEntry entry{};
    REQUIRE(FeatherOS::Files::ReadDirectory("/", 5, &entry) == 0);
    REQUIRE(std::string(entry.name) == "a");
    REQUIRE(entry.directory);
}

TEST_CASE("FAT16 stores long names with numbered short aliases")
{
    MakeFat16Disk("contents");
    REQUIRE(FeatherOS::VFS::MountBlockDevice());
    WriteWhole("/My Notes.txt", "first");
    WriteWhole("/My Notes2.txt", "second");
    const uint8_t* root = disk[RootDirectorySector].data();
    // Slot 1 holds the long-name entry and slot 2 the short alias.
    REQUIRE(root[32 + 11] == FeatherOS::FatNames::LongNameAttribute);
    REQUIRE(root[32] == 0x41);
    REQUIRE(std::memcmp(root + 64, "MYNOTE~1TXT", 11) == 0);
    REQUIRE(root[32 + 13] == FeatherOS::FatNames::Checksum(root + 64));
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
    const std::vector<uint8_t> archive = MakeTarArchive("hello.txt", "archive");
    REQUIRE(FeatherOS::Initramfs::Mount(archive.data(), archive.size()));
    REQUIRE(FeatherOS::VFS::IsDirectory("/"));
    REQUIRE(ListDirectory("/") ==
            std::vector<std::string>{"system/", "dev/", "proc/", "tmp/", "BLOCK.TXT"});
    REQUIRE(ListDirectory("/system") == std::vector<std::string>{"hello.txt"});
    REQUIRE(ReadWhole("/system/hello.txt") == "archive");
    FeatherOS::Files::Table table{};
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
    const std::vector<uint8_t> archive = MakeTarArchive("hello.txt", "archive");
    REQUIRE(FeatherOS::Initramfs::Mount(archive.data(), archive.size()));
    FeatherOS::Files::Table table{};
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
    REQUIRE(FeatherOS::Files::RemoveDirectory("/docs") == 0);
    REQUIRE(FeatherOS::Files::Seek(&table, 9, 0, FeatherOS::User::SeekFromStart) ==
            FeatherOS::User::SyscallErrorBadDescriptor);
}

TEST_CASE("stat reports type, size, and whether a file is read-only")
{
    MakeFat16Disk("contents");
    REQUIRE(FeatherOS::VFS::MountBlockDevice());
    const std::vector<uint8_t> archive = MakeTarArchive("hello.txt", "archive");
    REQUIRE(FeatherOS::Initramfs::Mount(archive.data(), archive.size()));
    FeatherOS::User::FileStatus status{};
    REQUIRE(FeatherOS::Files::Stat("/BLOCK.TXT", &status) == 0);
    REQUIRE(status.type == FeatherOS::User::FileType::Regular);
    REQUIRE(status.size == 8);
    REQUIRE_FALSE(status.read_only);
    REQUIRE(FeatherOS::Files::Stat("/system/hello.txt", &status) == 0);
    REQUIRE(status.type == FeatherOS::User::FileType::Regular);
    REQUIRE(status.size == 7);
    REQUIRE(status.read_only);
    REQUIRE(FeatherOS::Files::Stat("/", &status) == 0);
    REQUIRE(status.type == FeatherOS::User::FileType::Directory);
    REQUIRE_FALSE(status.read_only);
    REQUIRE(FeatherOS::Files::Stat("/system", &status) == 0);
    REQUIRE(status.type == FeatherOS::User::FileType::Directory);
    REQUIRE(status.read_only);
    REQUIRE(FeatherOS::Files::Stat("/missing", &status) == FeatherOS::User::SyscallErrorNotFound);

    FeatherOS::Files::Table table{};
    const uint64_t writer = FeatherOS::Files::Open(&table, "/BLOCK.TXT", OpenWrite | OpenAppend);
    REQUIRE(FeatherOS::Files::Write(&table, writer, "++", 2) == 2);
    REQUIRE(FeatherOS::Files::StatDescriptor(&table, writer, &status) == 0);
    REQUIRE(status.size == 10);
    REQUIRE_FALSE(status.read_only);
    REQUIRE(FeatherOS::Files::StatDescriptor(&table, writer + 1, &status) ==
            FeatherOS::User::SyscallErrorBadDescriptor);
    FeatherOS::Files::CloseAll(&table);
}
