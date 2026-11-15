// SPDX-License-Identifier: MIT OR Apache-2.0
#ifndef FEATHERDISK_TESTS_PARTITION_IMAGE_H
#define FEATHERDISK_TESTS_PARTITION_IMAGE_H

#include "tests/memory_device.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace FeatherDisk::Tests
{

using Guid = std::array<std::uint8_t, 16>;

// Any type GUID will do for the parser, which takes every used entry.
constexpr Guid ArbitraryType = {0xA2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x77};
// 0FC63DAF-8483-4772-8E79-3D69D8477DE4 as stored: the first three fields little-endian.
constexpr Guid LinuxFilesystemType = {0xAF, 0x3D, 0xC6, 0x0F, 0x83, 0x84, 0x72, 0x47,
                                      0x8E, 0x79, 0x3D, 0x69, 0xD8, 0x47, 0x7D, 0xE4};

void SetMbrEntry(MemoryDevice& device, std::size_t index, std::uint8_t type, std::uint32_t first,
                 std::uint32_t count);

// Written independently of the code under test: bit by bit, without its masking trick.
std::uint32_t Crc32(const std::uint8_t* bytes, std::size_t length);

struct GptEntry
{
    std::size_t index;
    std::uint64_t first;
    std::uint64_t last;
    Guid type = ArbitraryType;
};

struct GptLayout
{
    std::uint32_t entry_count = 128;
    std::uint32_t entry_size = 128;
};

std::uint8_t* GptHeader(MemoryDevice& device);
void SealGptHeader(MemoryDevice& device);
// A protective MBR, the GPT header in sector 1 and its entry array from sector 2. Entries not
// listed are unused (an all-zero type GUID).
void WriteGpt(MemoryDevice& device, const std::vector<GptEntry>& entries, GptLayout layout = {});

} // namespace FeatherDisk::Tests

#endif
