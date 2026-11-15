// SPDX-License-Identifier: MIT OR Apache-2.0
#ifndef FEATHERDISK_FSCLI_DISK_H
#define FEATHERDISK_FSCLI_DISK_H

#include "fs/filesystem.h"
#include "fs/partition.h"

#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace FeatherDisk::Fscli
{

// Every filesystem fscli can detect, in the order they are probed.
std::span<FilesystemType* const> FilesystemTypes();
FilesystemType* Detect(BlockDevice& volume);

std::string_view Describe(Error error);
std::string_view Describe(Partition::Scheme scheme);

struct Layout
{
    Partition::Scheme scheme;
    // A filesystem on the whole device, without a partition table; partitions is then empty.
    bool whole_device;
    std::vector<Partition::Range> partitions;
};

Layout ReadLayout(BlockDevice& device);

// A volume with its filesystem mounted until the object goes away.
class MountedVolume
{
public:
    // The error is a message for the user. Partitions are numbered from 1, as `info` lists them;
    // without a number, the whole device if it holds a filesystem, otherwise the first partition
    // that does.
    static std::expected<std::unique_ptr<MountedVolume>, std::string>
    Open(BlockDevice& device, std::optional<std::uint32_t> partition, MountOptions options);
    static std::expected<std::unique_ptr<MountedVolume>, std::string>
    Open(BlockDevice& device, Partition::Range range, MountOptions options);
    ~MountedVolume();
    MountedVolume(const MountedVolume&) = delete;
    MountedVolume& operator=(const MountedVolume&) = delete;

    Filesystem& Mounted();
    FilesystemType& Type();
    // Path components are separated by '/'; an empty path is the root.
    Result<NodeId> Walk(std::string_view path);

private:
    MountedVolume(BlockDevice& device, Partition::Range range);

    Partition::Volume volume;
    FilesystemType* type = nullptr;
    Filesystem* filesystem = nullptr;
};

} // namespace FeatherDisk::Fscli

#endif
