#ifndef FEATHEROS_FILESYSTEM_DRIVERS_H
#define FEATHEROS_FILESYSTEM_DRIVERS_H

#include "kernel/filesystem/core_driver.h"
#include "kernel/filesystem/filesystem.h"

#include "fs/partition.h"
#include "kernel/storage/disk.h"

#include <span>
#include <stddef.h>

// The built-in filesystems as drivers, one instance per mount; the disk's are one per partition.
namespace FeatherOS::Filesystem::Drivers
{

// A disk's sources, as Linux names the first virtio disk and its partitions: "vda" for the whole
// disk (number 0), "vda1" for the partition Partition::Find numbers 1, and so on; null past
// the last partition CoreDriver mounts.
const char* PartitionSource(size_t number);
// The boot disk's partitions are numbered 1 to MaximumVolumes, and each number has a driver of its
// own for the kernel's whole life, so a driver never dangles or changes volume. Sixteen more
// drivers, equally permanent, serve the volumes of every other disk as OpenDiskVolumes allocates
// them.
constexpr size_t MaximumVolumes = 16;
constexpr size_t MaximumOtherVolumes = 16;
constexpr size_t MaximumSourceLength = 16;
// The driver of the boot disk's partition number, empty until OpenVolume gives it a filesystem;
// null outside 1 to MaximumVolumes.
CoreDriver* Volume(size_t number);
// The volume driver that driver is, of either kind, or null for any other.
CoreDriver* VolumeOf(const Driver* driver);
// Makes the storage layer tell this module about disks that come and go: the worker scans a new
// disk's volumes, a removed disk's volumes die, /proc/partitions lists the volumes.
void AttachStorage();
// Opens the volumes of a disk other than the boot disk read-only in the core, a filesystem on the
// whole disk first, else each partition Partition::Find lists, and registers each under its type
// and name for mount; returns how many opened. Runs on the storage worker, which does disk I/O.
size_t OpenDiskVolumes(Storage::Disk& disk);
// Closes every volume of disk that is not mounted and frees its slot; a mounted one of a gone disk
// stays dead until Unmounted. Any task holding no volume lock.
void ForgetDiskVolumes(Storage::Disk& disk);
// Marks every volume of disk as being ejected (mounts of it fail), or clears the mark. Any task
// holding no volume lock.
void SetDiskEjecting(Storage::Disk& disk, bool ejecting);
// Whether source ("sda", "sda1") names a disk that was ejected and is still registered.
bool SourceOnEjectedDisk(const char* source);
// What mount calls the driver's volume, "vda1" or "sda2"; null for a driver with none.
const char* SourceOf(const Driver* driver);
// After the VFS removed driver's mount: a dead volume's slot is freed.
void Unmounted(Driver* driver);
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
// The RAM filesystem's roots: /tmp, /etc, and the root of a machine with no disk.
Driver& Tmp();
Driver& Etc();
Driver& RamRoot();

} // namespace FeatherOS::Filesystem::Drivers

#endif
