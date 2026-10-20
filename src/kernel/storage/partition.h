#ifndef FEATHEROS_STORAGE_PARTITION_H
#define FEATHEROS_STORAGE_PARTITION_H

#include <stdint.h>

namespace FeatherOS::Partition
{

struct Range
{
    uint64_t first_sector;
    uint64_t sector_count;
};

constexpr uint32_t MaximumPartitions = 16;

// Partitions that may hold a filesystem: the used entries of a GUID partition table when the MBR
// is protective, otherwise the MBR's FAT16 entries. A GPT whose header or entry array fails its
// CRC32 check yields no partitions rather than falling back to the MBR.
uint32_t Find(Range (&ranges)[MaximumPartitions]);

} // namespace FeatherOS::Partition

#endif
