// SPDX-License-Identifier: MIT OR Apache-2.0
// The ext2 images tests/make_ext2_image.sh makes, and e2fsprogs to check what the core wrote.
#ifndef FEATHERDISK_TESTS_EXT2_IMAGE_H
#define FEATHERDISK_TESTS_EXT2_IMAGE_H

#include "tests/memory_device.h"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace FeatherDisk::Tests
{

constexpr std::uint32_t ImageSectorSize = 512;

// Only with FEATHERDISK_EXT_IMAGES.
std::vector<std::uint8_t> LoadExtImage(std::string_view variant);
std::unique_ptr<MemoryDevice> DeviceFrom(const std::vector<std::uint8_t>& bytes,
                                         std::uint32_t sector_size = ImageSectorSize);
std::unique_ptr<MemoryDevice> CopyOf(MemoryDevice& device);
std::vector<std::uint8_t> BytesOf(MemoryDevice& device);

// The volume's structures located from its bytes alone, to read or damage them on purpose.
struct RawVolume
{
    std::vector<std::uint8_t>& bytes;

    std::uint16_t Load16(std::uint64_t offset) const;
    std::uint32_t Load32(std::uint64_t offset) const;
    void Store16(std::uint64_t offset, std::uint16_t value);
    void Store32(std::uint64_t offset, std::uint32_t value);
    std::uint32_t BlockSize() const;
    std::uint32_t BlockCount() const;
    std::uint32_t InodeCount() const;
    std::uint64_t BlockOffset(std::uint32_t block) const;
    std::uint64_t DescriptorOffset() const;
    std::uint64_t InodeOffset(std::uint32_t inode) const;
    std::uint32_t InodeBlock(std::uint32_t inode, std::uint32_t index) const;
    // The named entry in the directory's first block.
    std::uint64_t EntryOffset(std::uint32_t directory, std::string_view name) const;
};

struct ToolRun
{
    bool ran;
    int status;
    std::string output;
};

// Whether e2fsck was found; without it, CheckClean passes and checks nothing.
bool HaveE2fsck();
// e2fsck -fn over the device's bytes, saved as <name>.img in the test output directory, which
// must exit 0: nothing to report.
void CheckClean(MemoryDevice& device, std::string_view name);
// e2fsck -fn's exit status and output, unchecked.
ToolRun RunE2fsck(MemoryDevice& device, std::string_view name);
// debugfs -R command over the device's bytes: what it printed on standard output.
ToolRun RunDebugfs(MemoryDevice& device, std::string_view name, std::string_view command);
// debugfs -w -R command, with the changed volume read back into the device.
ToolRun ChangeWithDebugfs(MemoryDevice& device, std::string_view name, std::string_view command);

} // namespace FeatherDisk::Tests

#endif
