#ifndef FEATHEROS_FILESYSTEM_DRIVERS_H
#define FEATHEROS_FILESYSTEM_DRIVERS_H

#include "kernel/filesystem/core_driver.h"
#include "kernel/filesystem/filesystem.h"

#include "fs/partition.h"

#include <span>
#include <stddef.h>

// The built-in filesystems as drivers, one instance per mount; the disk's are one per partition.
namespace FeatherOS::Filesystem::Drivers
{

// A disk's sources, as Linux names the first virtio disk and its partitions: "vda" for the whole
// disk (number 0), "vda1" for the partition Partition::Find numbers 1, and so on; null past
// the last partition CoreDriver mounts.
const char* PartitionSource(size_t number);
// Partitions are numbered 1 to MaximumVolumes, and each number has a driver of its own for the
// kernel's whole life, so a driver never dangles or changes volume.
constexpr size_t MaximumVolumes = 16;
// The driver of partition number, empty until OpenVolume gives it a filesystem; null outside 1 to
// MaximumVolumes.
CoreDriver* Volume(size_t number);
// The volume driver that driver is, or null for any other.
CoreDriver* VolumeOf(const Driver* driver);
// The block device's partitions that may hold a filesystem, in Partition::Find's order, at most
// MaximumVolumes of them; returns how many.
size_t FindPartitions(std::span<FeatherDisk::Partition::Range> ranges);
// Opens range's filesystem read-only in the core in Volume(range.number), as the first of FAT16,
// ext2, ext3 and ext4 whose probe and mount accept it. False, with the volume empty, if none
// does, if the number is out of range or if the volume's previous filesystem could not be closed.
bool OpenVolume(const FeatherDisk::Partition::Range& range);
// Closes number's filesystem and leaves its driver empty; false, unchanged, if the core cannot
// close it.
bool ForgetVolume(size_t number);
// The initramfs's "system" directory.
Driver& System();
Driver& Devices();
Driver& Proc();
// The RAM filesystem's two roots.
Driver& Tmp();
Driver& Etc();

} // namespace FeatherOS::Filesystem::Drivers

#endif
