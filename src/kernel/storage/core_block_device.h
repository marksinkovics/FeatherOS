#ifndef FEATHEROS_STORAGE_CORE_BLOCK_DEVICE_H
#define FEATHEROS_STORAGE_CORE_BLOCK_DEVICE_H

#include "fs/block_device.h"

namespace FeatherOS::BlockDevice
{

// The boot disk (vda) as the shared filesystem core (src/fs/core/) sees it; it must be registered.
FeatherDisk::BlockDevice& CoreDevice();

} // namespace FeatherOS::BlockDevice

#endif
