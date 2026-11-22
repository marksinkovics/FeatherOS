// SPDX-License-Identifier: MIT OR Apache-2.0
#ifndef FEATHERDISK_TESTS_FAT_IMAGE_H
#define FEATHERDISK_TESTS_FAT_IMAGE_H

#include "tests/filesystem_helpers.h"
#include "tests/memory_device.h"

#include <cstdint>
#include <string>

namespace FeatherDisk::Tests
{

// A small FAT16 volume without a partition table: one reserved sector, the FATs, the root
// directory, then one-sector clusters.
struct Fat16Layout
{
    std::uint32_t sectors = 5000;
    std::uint32_t fat_count = 1;
    std::uint32_t fat_sectors = 20;
    std::uint32_t root_entries = 16;
};

class Fat16Image
{
public:
    explicit Fat16Image(Fat16Layout image_layout = {});

    MemoryDevice& Device();
    std::uint32_t RootDirectorySector() const;
    std::uint8_t* RootEntry(std::uint32_t index);
    std::uint16_t FatEntry(std::uint32_t cluster, std::uint32_t copy = 0);
    // In every FAT copy.
    void SetFatEntry(std::uint32_t cluster, std::uint16_t value);
    // BLOCK.TXT in the root's first entry, in cluster 2 and, past 512 bytes, cluster 3.
    void AddRootFile(const std::string& contents);

private:
    Fat16Layout layout;
    MemoryDevice device;
};

} // namespace FeatherDisk::Tests

#endif
