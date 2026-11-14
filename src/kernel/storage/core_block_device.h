#ifndef FEATHEROS_STORAGE_CORE_BLOCK_DEVICE_H
#define FEATHEROS_STORAGE_CORE_BLOCK_DEVICE_H

#include "fs/block_device.h"

namespace FeatherOS::BlockDevice
{

// The virtio disk as the shared filesystem core (src/fs/core/) sees it.
FeatherDisk::BlockDevice& CoreDevice();

} // namespace FeatherOS::BlockDevice

#endif
