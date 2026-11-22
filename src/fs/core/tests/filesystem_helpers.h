// SPDX-License-Identifier: MIT OR Apache-2.0
#ifndef FEATHERDISK_TESTS_FILESYSTEM_HELPERS_H
#define FEATHERDISK_TESTS_FILESYSTEM_HELPERS_H

#include "fs/filesystem.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace FeatherDisk::Tests
{

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
// stem, the number and letters up to length bytes.
std::string NumberedName(std::string_view stem, int number, std::size_t length);
std::span<const std::uint8_t> BytesOfText(const std::string& text);
std::size_t WriteAt(Filesystem& filesystem, NodeId file, std::uint64_t offset,
                    const std::string& text);
std::string ReadAt(Filesystem& filesystem, NodeId file, std::uint64_t offset, std::size_t length);
std::uint32_t FreeBlocks(Filesystem& filesystem);

} // namespace FeatherDisk::Tests

#endif
