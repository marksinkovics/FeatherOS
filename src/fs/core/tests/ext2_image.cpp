// SPDX-License-Identifier: MIT OR Apache-2.0
#include "tests/ext2_image.h"

#include "common/crc.h"
#include "common/little_endian.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <sys/wait.h>
#include <unistd.h>

namespace FeatherDisk::Tests
{
namespace
{

std::string ReadHostFile(const std::string& path)
{
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

// Each process has its own files, since CTest may run test cases side by side.
std::string WorkPath(std::string_view name)
{
#ifdef FEATHERDISK_EXT_WORK
    return std::string(FEATHERDISK_EXT_WORK) + "/" + std::string(name) + "-" +
           std::to_string(::getpid());
#else
    return std::string(name);
#endif
}

void SaveImage(MemoryDevice& device, const std::string& path)
{
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    const std::span<std::uint8_t> bytes = device.Bytes();
    file.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
    REQUIRE(file.good());
}

[[maybe_unused]] ToolRun Run(const std::string& command, const std::string& output)
{
    const int result = std::system(command.c_str());
    ToolRun run = {.ran = true, .status = -1, .output = ReadHostFile(output)};
    if (result != -1 && WIFEXITED(result))
    {
        run.status = WEXITSTATUS(result);
    }
    return run;
}

ToolRun Debugfs([[maybe_unused]] MemoryDevice& device, [[maybe_unused]] std::string_view name,
                [[maybe_unused]] std::string_view command, [[maybe_unused]] bool writable)
{
#ifdef FEATHERDISK_DEBUGFS
    const std::string image = WorkPath(name) + ".img";
    const std::string output = image + ".debugfs.txt";
    SaveImage(device, image);
    const ToolRun run =
        Run(std::string("\"") + FEATHERDISK_DEBUGFS + "\"" + (writable ? " -w" : "") + " -R '" +
                std::string(command) + "' \"" + image + "\" > \"" + output + "\" 2>/dev/null",
            output);
    if (writable)
    {
        std::ifstream file(image, std::ios::binary);
        file.read(reinterpret_cast<char*>(device.Bytes().data()),
                  static_cast<std::streamsize>(device.Bytes().size()));
        REQUIRE(file.good());
    }
    std::remove(output.c_str());
    std::remove(image.c_str());
    return run;
#else
    return {.ran = false, .status = 0, .output = {}};
#endif
}

} // namespace

std::vector<std::uint8_t> LoadExtImage(std::string_view variant)
{
#ifdef FEATHERDISK_EXT_IMAGES
    const std::string path =
        std::string(FEATHERDISK_EXT_IMAGES) + "/" + std::string(variant) + ".img";
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    REQUIRE(file.good());
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(file.tellg()));
    file.seekg(0);
    file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    REQUIRE(file.good());
    REQUIRE(bytes.size() % 4096 == 0);
    return bytes;
#else
    FAIL("no ext2 images for " << variant);
    return {};
#endif
}

std::unique_ptr<MemoryDevice> DeviceFrom(const std::vector<std::uint8_t>& bytes,
                                         std::uint32_t sector_size)
{
    auto device = std::make_unique<MemoryDevice>(sector_size, bytes.size() / sector_size);
    std::memcpy(device->Bytes().data(), bytes.data(), device->Bytes().size());
    return device;
}

std::unique_ptr<MemoryDevice> CopyOf(MemoryDevice& device)
{
    auto copy = std::make_unique<MemoryDevice>(device.SectorSize(), device.SectorCount());
    std::memcpy(copy->Bytes().data(), device.Bytes().data(), device.Bytes().size());
    return copy;
}

std::vector<std::uint8_t> BytesOf(MemoryDevice& device)
{
    return {device.Bytes().begin(), device.Bytes().end()};
}

std::uint16_t RawVolume::Load16(std::uint64_t offset) const
{
    return LoadLittleEndian<std::uint16_t>(bytes.data() + offset);
}

std::uint32_t RawVolume::Load32(std::uint64_t offset) const
{
    return LoadLittleEndian<std::uint32_t>(bytes.data() + offset);
}

void RawVolume::Store16(std::uint64_t offset, std::uint16_t value)
{
    StoreLittleEndian(bytes.data() + offset, value);
}

void RawVolume::Store32(std::uint64_t offset, std::uint32_t value)
{
    StoreLittleEndian(bytes.data() + offset, value);
}

std::uint32_t RawVolume::BlockSize() const
{
    return 1024U << Load32(1024 + 24);
}

std::uint32_t RawVolume::BlockCount() const
{
    return Load32(1024 + 4);
}

std::uint32_t RawVolume::InodeCount() const
{
    return Load32(1024 + 0);
}

std::uint64_t RawVolume::BlockOffset(std::uint32_t block) const
{
    return std::uint64_t{block} * BlockSize();
}

std::uint64_t RawVolume::DescriptorOffset() const
{
    return BlockOffset(Load32(1024 + 20) + 1);
}

std::uint32_t RawVolume::DescriptorSize() const
{
    return (Load32(1024 + 96) & 0x80U) != 0 ? Load16(1024 + 254) : 32;
}

std::uint32_t RawVolume::InodeSize() const
{
    return Load32(1024 + 76) == 0 ? 128 : Load16(1024 + 88);
}

std::uint64_t RawVolume::InodeOffset(std::uint32_t inode) const
{
    const std::uint32_t per_group = Load32(1024 + 40);
    const std::uint32_t group = (inode - 1) / per_group;
    const std::uint32_t table = Load32(DescriptorOffset() + group * DescriptorSize() + 8);
    return BlockOffset(table) + std::uint64_t{(inode - 1) % per_group} * InodeSize();
}

void RawVolume::UpdateSuperblockChecksum()
{
    Store32(1024 + 1020, Crc32c(~0U, std::span(bytes).subspan(1024, 1020)));
}

std::uint32_t RawVolume::InodeBlock(std::uint32_t inode, std::uint32_t index) const
{
    return Load32(InodeOffset(inode) + 40 + index * 4);
}

std::vector<std::uint64_t> RawVolume::ExtentPath(std::uint32_t inode, std::uint32_t logical) const
{
    std::vector<std::uint64_t> path;
    std::uint64_t node = InodeOffset(inode) + 40;
    REQUIRE(Load16(node) == 0xF30A);
    while (Load16(node + 6) > 0)
    {
        const std::uint16_t entries = Load16(node + 2);
        REQUIRE(entries > 0);
        std::uint64_t entry = node + 12;
        for (std::uint16_t index = 1; index < entries && Load32(node + 12 + index * 12) <= logical;
             ++index)
        {
            entry = node + 12 + index * 12;
        }
        const std::uint64_t child = Load32(entry + 4) | (std::uint64_t{Load16(entry + 8)} << 32);
        path.push_back(child);
        node = child * BlockSize();
        REQUIRE(Load16(node) == 0xF30A);
    }
    return path;
}

std::uint64_t RawVolume::FileBlock(std::uint32_t inode, std::uint32_t logical) const
{
    if ((Load32(InodeOffset(inode) + 32) & 0x80000U) == 0)
    {
        REQUIRE(logical < 12);
        return InodeBlock(inode, logical);
    }
    const std::vector<std::uint64_t> path = ExtentPath(inode, logical);
    const std::uint64_t node = path.empty() ? InodeOffset(inode) + 40 : path.back() * BlockSize();
    for (std::uint16_t index = 0; index < Load16(node + 2); ++index)
    {
        const std::uint64_t entry = node + 12 + std::uint64_t{index} * 12;
        const std::uint32_t first = Load32(entry);
        std::uint32_t length = Load16(entry + 4);
        length = length > 32768 ? length - 32768 : length;
        if (logical >= first && logical - first < length)
        {
            return ((std::uint64_t{Load16(entry + 6)} << 32) | Load32(entry + 8)) + logical - first;
        }
    }
    return 0;
}

std::uint64_t RawVolume::EntryOffset(std::uint32_t directory, std::string_view name) const
{
    const std::uint64_t block = FileBlock(directory, 0) * BlockSize();
    for (std::uint64_t offset = 0; offset < BlockSize();)
    {
        const std::uint16_t length = Load16(block + offset + 4);
        const std::string_view entry_name(
            reinterpret_cast<const char*>(bytes.data() + block + offset + 8),
            bytes[block + offset + 6]);
        if (entry_name == name)
        {
            return block + offset;
        }
        REQUIRE(length != 0);
        offset += length;
    }
    FAIL("no entry " << name);
    return 0;
}

bool HaveE2fsck()
{
#ifdef FEATHERDISK_E2FSCK
    return true;
#else
    return false;
#endif
}

ToolRun RunE2fsck([[maybe_unused]] MemoryDevice& device, [[maybe_unused]] std::string_view name)
{
#ifdef FEATHERDISK_E2FSCK
    const std::string image = WorkPath(name) + ".img";
    const std::string output = image + ".e2fsck.txt";
    SaveImage(device, image);
    const ToolRun run = Run(std::string("\"") + FEATHERDISK_E2FSCK + "\" -fn \"" + image +
                                "\" > \"" + output + "\" 2>&1",
                            output);
    std::remove(output.c_str());
    std::remove(image.c_str());
    return run;
#else
    return {.ran = false, .status = 0, .output = {}};
#endif
}

void CheckClean(MemoryDevice& device, std::string_view name)
{
    const ToolRun run = RunE2fsck(device, name);
    INFO("e2fsck -fn on " << name << ":\n" << run.output);
    REQUIRE(run.status == 0);
}

// A '+' in a bitmap difference would be something in use that its bitmap calls free.
bool OnlyRoutineRepairs(const std::string& report, bool renames)
{
    constexpr std::array<std::string_view, 12> Routine = {
        "Free blocks count wrong",
        "Free inodes count wrong",
        "Directories count wrong",
        "Unattached inode",
        "Unattached zero-length inode",
        "Unconnected directory inode",
        // The unconnected directory's ".." against the parent e2fsck did not find.
        "should be <The NULL inode> (0)",
        "ref count is",
        ", i_size is",
        ", i_blocks is",
        "Block bitmap differences:",
        "Inode bitmap differences:",
    };
    constexpr std::array<std::string_view, 2> Renames = {
        "is a link to directory",
        "'..' in ",
    };
    constexpr std::array<std::string_view, 8> Noise = {
        "e2fsck ",
        "Pass ",
        "Fix? no",
        "Clear? no",
        "Connect to /lost+found? no",
        "WARNING: Filesystem still has errors",
        " files (",
        "check forced",
    };
    const auto contains = [](std::string_view line, const auto& phrases)
    {
        return std::any_of(phrases.begin(), phrases.end(), [&](std::string_view phrase)
                           { return line.find(phrase) != std::string_view::npos; });
    };
    std::size_t start = 0;
    while (start < report.size())
    {
        std::size_t end = report.find('\n', start);
        end = end == std::string::npos ? report.size() : end;
        const std::string_view line(report.data() + start, end - start);
        start = end + 1;
        if (line.empty() || contains(line, Noise))
        {
            continue;
        }
        if (!contains(line, Routine) && !(renames && contains(line, Renames)))
        {
            return false;
        }
        if (line.find("bitmap differences") != std::string_view::npos &&
            line.find('+') != std::string_view::npos)
        {
            return false;
        }
    }
    return true;
}

ToolRun RunDebugfs(MemoryDevice& device, std::string_view name, std::string_view command)
{
    return Debugfs(device, name, command, false);
}

ToolRun ChangeWithDebugfs(MemoryDevice& device, std::string_view name, std::string_view command)
{
    return Debugfs(device, name, command, true);
}

} // namespace FeatherDisk::Tests
