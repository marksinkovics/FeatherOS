#ifndef FEATHEROS_FILESYSTEM_VFS_H
#define FEATHEROS_FILESYSTEM_VFS_H

#include "kernel/filesystem/filesystem.h"

#include <stdint.h>

// One tree over the mounted filesystems: at boot the FAT16 disk at "/", the read-only initramfs's
// "system" directory at "/system", devices at "/dev", generated process files at "/proc", and the
// RAM filesystem's two roots at "/tmp" and "/etc". A path belongs to the mount with the longest
// mount point that is a whole-component prefix of it, so a mount hides whatever its parent
// filesystem has at that path.
namespace FeatherOS::VFS
{

using FileInfo = Filesystem::FileInfo;

struct FileHandle
{
    Filesystem::Driver* driver;
    Filesystem::Node node;
    uint64_t size;
    uint64_t offset;
    bool open;
};

// Names one file across all its handles.
struct FileKey
{
    const Filesystem::Driver* filesystem;
    uint64_t node;

    bool operator==(const FileKey&) const = default;
};

// The filesystems that need no disk, in the order "/" lists them: /system, /etc, /dev, /proc and
// /tmp. Those already mounted are left alone.
bool MountBuiltIn();
// Mounts the block device's FAT16 volume at "/", replacing an earlier one.
bool MountBlockDevice();
// Copies the initramfs's "etc" files into /etc, so they can be changed until the next boot.
bool LoadEtc();
// point is an absolute path without ".", "..", repeated or trailing '/'; it fails if taken.
bool Mount(const char* point, Filesystem::Driver* driver);
bool Unmount(const char* point);

bool Open(const char* path, FileHandle* file);
bool Read(FileHandle* file, void* buffer, uint64_t length, uint64_t* bytes_read);
// Devices accept any offset; other files at most their size.
bool Seek(FileHandle* file, uint64_t offset);
bool Stat(const char* path, FileInfo* info);
void Close(FileHandle* file);
// Disk, /tmp and /etc files can be created and changed, and devices take writes. A handle caches
// the file's size, so other handles to the same file do not see its changes.
bool Create(const char* path, FileHandle* file);
// Writes at the handle's offset and advances it; *bytes_written may be short if space runs out.
bool Write(FileHandle* file, const void* buffer, uint64_t length, uint64_t* bytes_written);
bool Truncate(FileHandle* file, uint64_t size);
bool Remove(const char* path);
bool MakeDirectory(const char* path);
bool IsDirectory(const char* path);
// True where nothing can be created or changed: /system and /proc, the bare names kernel code
// uses for the initramfs, and /dev apart from its devices.
bool IsReadOnly(const char* path);
// "/" and the mount points, which can never be removed.
bool IsMountPoint(const char* path);
// Disk, /tmp and /etc files are tracked while open, so they are not removed or written twice at
// once.
bool IsTracked(const FileHandle& file);
FileKey KeyOf(const FileHandle& file);
bool IsWritable(const FileHandle& file);
bool IsDevice(const FileHandle& file);
// The node Devices::Find gave for a device; false for anything else.
bool DeviceNode(const FileHandle& file, uint32_t* node);
bool RemoveDirectory(const char* path);
bool SameFilesystem(const char* left, const char* right);
// Within one filesystem; to must not exist, unless it names from in another case on the disk.
bool Rename(const char* from, const char* to);
// Lists a directory, such as "/", "/home/docs", or "/system": first the mount points directly
// inside it, then its own entries. False past the last entry.
bool ReadDirectory(const char* path, uint32_t index, Filesystem::DirectoryEntry* entry);

} // namespace FeatherOS::VFS

#endif
