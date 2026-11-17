#ifndef FEATHEROS_FILESYSTEM_DRIVERS_H
#define FEATHEROS_FILESYSTEM_DRIVERS_H

#include "kernel/filesystem/core_driver.h"
#include "kernel/filesystem/filesystem.h"

#include <stddef.h>

// The built-in filesystems as drivers, one instance per mount.
namespace FeatherOS::Filesystem::Drivers
{

// A disk's sources, as Linux names the first virtio disk and its partitions: "vda" for the whole
// disk (number 0), "vda1" for the first partition Partition::Find lists, and so on; null past
// the last partition CoreDriver mounts.
const char* PartitionSource(size_t number);
// The block device's FAT16 filesystem, from the shared core.
CoreDriver& Disk();
// Mounts the first FAT16 partition on the block device as Disk().
bool MountDisk();
// The block device's ext2 filesystem, writable once mounted in the VFS without
// MountMode::ReadOnly.
CoreDriver& Ext2();
// Mounts the first ext2 partition on the block device as Ext2(), read-only in the core until the
// VFS mounts it read-write, so an unmounted volume stays clean.
bool MountExt2();
// The block device's ext3 and ext4 filesystems, which the core keeps read-only whatever the VFS
// asks for until it writes them.
CoreDriver& Ext3();
CoreDriver& Ext4();
// Mount the first ext3 or ext4 partition on the block device, read-only.
bool MountExt3();
bool MountExt4();
// The initramfs's "system" directory.
Driver& System();
Driver& Devices();
Driver& Proc();
// The RAM filesystem's two roots.
Driver& Tmp();
Driver& Etc();

} // namespace FeatherOS::Filesystem::Drivers

#endif
