#ifndef FEATHEROS_FILESYSTEM_DRIVERS_H
#define FEATHEROS_FILESYSTEM_DRIVERS_H

#include "kernel/filesystem/filesystem.h"

// The built-in filesystems as drivers, one instance per mount.
namespace FeatherOS::Filesystem::Drivers
{

// The FAT16 volume on the block device.
Driver& Disk();
// The initramfs's "system" directory.
Driver& System();
Driver& Devices();
Driver& Proc();
// The RAM filesystem's two roots.
Driver& Tmp();
Driver& Etc();

} // namespace FeatherOS::Filesystem::Drivers

#endif
