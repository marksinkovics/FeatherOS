#include "core_block_device.h"

#include "kernel/storage/disk.h"

namespace FeatherOS::BlockDevice
{

FeatherDisk::BlockDevice& CoreDevice()
{
    return *Storage::BootDisk();
}

} // namespace FeatherOS::BlockDevice
