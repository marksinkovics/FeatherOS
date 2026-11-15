#ifndef FEATHEROS_FILESYSTEM_DRIVERS_H
#define FEATHEROS_FILESYSTEM_DRIVERS_H

#include "kernel/filesystem/core_driver.h"
#include "kernel/filesystem/filesystem.h"

// The built-in filesystems as drivers, one instance per mount.
namespace FeatherOS::Filesystem::Drivers
{

// The block device's filesystem, from the shared core.
CoreDriver& Disk();
// Mounts the first FAT16 partition on the block device as Disk().
bool MountDisk();
// The initramfs's "system" directory.
Driver& System();
Driver& Devices();
Driver& Proc();
// The RAM filesystem's two roots.
Driver& Tmp();
Driver& Etc();

} // namespace FeatherOS::Filesystem::Drivers

#endif
