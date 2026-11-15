// SPDX-License-Identifier: MIT OR Apache-2.0
#ifndef FEATHERDISK_TESTS_FAT_IMAGE_H
#define FEATHERDISK_TESTS_FAT_IMAGE_H

#include "fs/filesystem.h"
#include "tests/memory_device.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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

// Mounted for the length of a test.
class MountedFilesystem
{
public:
    MountedFilesystem(FilesystemType& filesystem_type, BlockDevice& volume, bool read_only = false);
    ~MountedFilesystem();
    MountedFilesystem(const MountedFilesystem&) = delete;
    MountedFilesystem& operator=(const MountedFilesystem&) = delete;

    Filesystem& operator*();
    Filesystem* operator->();

private:
    FilesystemType& type;
    Filesystem* filesystem;
};

template <typename T> T Value(Result<T> result)
{
    REQUIRE(result.has_value());
    return *std::move(result);
}

inline void Succeeds(const Status& status)
{
    REQUIRE(status.has_value());
}

template <typename T> Error ErrorOf(const Result<T>& result)
{
    REQUIRE_FALSE(result.has_value());
    return result.error();
}

// Each '/'-separated component looked up in turn from the root.
Result<NodeId> Walk(Filesystem& filesystem, std::string_view path);
// The directory holding the last component of path, and that component.
std::pair<NodeId, std::string> Parent(Filesystem& filesystem, std::string_view path);
std::string ReadAll(Filesystem& filesystem, NodeId file);
NodeId WriteNewFile(Filesystem& filesystem, std::string_view path, const std::string& contents);
// Directories end in '/'.
std::vector<std::string> List(Filesystem& filesystem, NodeId directory);
std::vector<std::string> Sorted(std::vector<std::string> names);
std::string Pattern(std::size_t length);
std::uint32_t FreeBlocks(Filesystem& filesystem);

} // namespace FeatherDisk::Tests

#endif
