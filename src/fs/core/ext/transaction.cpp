// SPDX-License-Identifier: MIT OR Apache-2.0
// Where blocks go. Metadata and file data are told apart at every access so that a journal can
// hold the metadata; for now both reach the block cache and the device as they always did.
#include "ext/ext2_filesystem.h"

#include <cstddef>
#include <cstdint>
#include <span>

namespace FeatherDisk::Ext
{

Status Ext2Filesystem::ReadMetadata(std::uint64_t block, std::size_t offset,
                                    std::span<std::uint8_t> out, std::span<std::uint8_t> scratch)
{
    return cache.Read(block, offset, out, scratch);
}

Status Ext2Filesystem::WriteMetadata(std::uint64_t first_block, std::span<const std::uint8_t> bytes)
{
    return cache.Write(first_block, bytes);
}

Status Ext2Filesystem::WriteData(std::uint64_t first_block, std::span<const std::uint8_t> bytes)
{
    return cache.Write(first_block, bytes);
}

} // namespace FeatherDisk::Ext
