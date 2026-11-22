#ifndef FEATHEROS_STORAGE_CORE_BLOCK_DEVICE_H
#define FEATHEROS_STORAGE_CORE_BLOCK_DEVICE_H

#include "fs/block_device.h"

#include <stdint.h>

namespace FeatherOS::BlockDevice
{

// The virtio disk as the shared filesystem core (src/fs/core/) sees it.
FeatherDisk::BlockDevice& CoreDevice();

// Counted in the virtio disk, below any cache.
struct DeviceCounters
{
    uint64_t read_sectors;
    uint64_t written_sectors;
    uint64_t flushes;
};

DeviceCounters DeviceStatistics();

} // namespace FeatherOS::BlockDevice

#endif
