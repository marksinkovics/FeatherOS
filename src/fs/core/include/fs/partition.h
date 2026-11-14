// SPDX-License-Identifier: MIT OR Apache-2.0
#ifndef FEATHERDISK_PARTITION_H
#define FEATHERDISK_PARTITION_H

#include "fs/block_device.h"

#include <cstddef>
#include <cstdint>
#include <span>

namespace FeatherDisk::Partition
{

struct Range
{
    std::uint64_t first_sector;
    std::uint64_t sector_count;
};

// Partitions that may hold a filesystem, in table order: the used entries of a GUID partition
// table when the MBR is protective, otherwise the MBR's FAT16 entries. A GPT whose header or
// entry array fails its CRC32 check yields none rather than falling back to the MBR. Returns how
// many were stored, at most ranges.size().
std::size_t Find(BlockDevice& device, std::span<Range> ranges);

} // namespace FeatherDisk::Partition

#endif
