// SPDX-License-Identifier: MIT OR Apache-2.0
#include "tests/ext2_image.h"

#include "common/little_endian.h"

#include <catch2/catch_test_macros.hpp>

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

std::uint64_t RawVolume::InodeOffset(std::uint32_t inode) const
{
    const std::uint32_t per_group = Load32(1024 + 40);
    const std::uint32_t size = Load32(1024 + 76) == 0 ? 128 : Load16(1024 + 88);
    const std::uint32_t group = (inode - 1) / per_group;
    const std::uint32_t table = Load32(DescriptorOffset() + group * 32 + 8);
    return BlockOffset(table) + std::uint64_t{(inode - 1) % per_group} * size;
}

std::uint32_t RawVolume::InodeBlock(std::uint32_t inode, std::uint32_t index) const
{
    return Load32(InodeOffset(inode) + 40 + index * 4);
}

std::uint64_t RawVolume::EntryOffset(std::uint32_t directory, std::string_view name) const
{
    const std::uint64_t block = BlockOffset(InodeBlock(directory, 0));
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

ToolRun RunDebugfs(MemoryDevice& device, std::string_view name, std::string_view command)
{
    return Debugfs(device, name, command, false);
}

ToolRun ChangeWithDebugfs(MemoryDevice& device, std::string_view name, std::string_view command)
{
    return Debugfs(device, name, command, true);
}

} // namespace FeatherDisk::Tests
