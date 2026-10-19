#include <catch2/catch_test_macros.hpp>

#include "kernel/filesystem/fat16.h"
#include "kernel/filesystem/initramfs.h"
#include "kernel/filesystem/vfs.h"
#include "kernel/storage/block_device.h"

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
    REQUIRE(FeatherOS::VFS::Stat("/disk/BLOCK.TXT", &info));
    REQUIRE(info.size == contents.size());

    FeatherOS::VFS::FileHandle file{};
    REQUIRE(FeatherOS::VFS::Open("/disk/BLOCK.TXT", &file));
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
    REQUIRE_FALSE(FeatherOS::VFS::Stat("/disk/MISSING.TXT", &info));
}

TEST_CASE("FAT16 VFS rejects corrupt cluster chains")
{
    MakeFat16Disk(std::string(700, 'x'));
    WriteLittleEndian16(disk[PartitionStart + 1].data() + 4, 0xFFF7);
    REQUIRE(FeatherOS::VFS::MountBlockDevice());
    FeatherOS::VFS::FileHandle file{};
    REQUIRE(FeatherOS::VFS::Open("/disk/BLOCK.TXT", &file));
    std::array<char, 700> contents{};
    uint64_t bytes_read = 0;
    REQUIRE_FALSE(FeatherOS::VFS::Read(&file, contents.data(), contents.size(), &bytes_read));
}

TEST_CASE("FAT16 VFS returns zero bytes at end of file")
{
    MakeFat16Disk("contents");
    REQUIRE(FeatherOS::VFS::MountBlockDevice());
    FeatherOS::VFS::FileHandle file{};
    REQUIRE(FeatherOS::VFS::Open("/disk/BLOCK.TXT", &file));
    FeatherOS::VFS::FileInfo info{};
    REQUIRE(FeatherOS::VFS::Stat("/disk/BLOCK.TXT", &info));
    REQUIRE(FeatherOS::VFS::Seek(&file, info.size));
    char byte;
    uint64_t bytes_read = 1;
    REQUIRE(FeatherOS::VFS::Read(&file, &byte, 1, &bytes_read));
    REQUIRE(bytes_read == 0);
}