#ifndef FEATHEROS_FILESYSTEM_VFS_H
#define FEATHEROS_FILESYSTEM_VFS_H

#include "kernel/filesystem/fat16.h"

#include <stdint.h>

// One tree over several filesystems: the FAT16 disk at "/", the read-only initramfs at
// "/system", devices at "/dev", generated process files at "/proc", and a RAM filesystem at
// "/tmp". Disk entries with those names in the root are hidden by the mounts.
namespace FeatherOS::VFS
{

enum class FileKind : uint8_t
{
    Archive,
    Disk,
    Device,
    Memory,
    Generated,
};

struct FileHandle
{
    // An archive file's bytes, or a generated file's text, which the handle owns.
    const uint8_t* data;
    uint64_t size;
    uint64_t offset;
    uint32_t first_cluster;
    uint32_t directory_cluster;
    uint32_t entry_index;
    // A tmpfs node, or a Devices::Device.
    uint32_t node;
    FileKind kind;
    bool open;
};

struct FileInfo
{
    uint64_t size;
    bool device;
};

bool MountBlockDevice();
bool Open(const char* path, FileHandle* file);
bool Read(FileHandle* file, void* buffer, uint64_t length, uint64_t* bytes_read);
// Devices accept any offset; other files at most their size.
bool Seek(FileHandle* file, uint64_t offset);
bool Stat(const char* path, FileInfo* info);
void Close(FileHandle* file);
// Disk and /tmp files can be created and changed, and devices take writes. A handle caches the
// file's size, so other handles to the same file do not see its changes.
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
// Disk and /tmp files are tracked while open, so they are not removed or written twice at once.
bool IsTracked(const FileHandle& file);
bool RemoveDirectory(const char* path);
// Lists a directory, such as "/", "/home/docs", or "/system"; "/" starts with the mount points.
// False past the last entry.
bool ReadDirectory(const char* path, uint32_t index, FAT16::DirectoryEntry* entry);

} // namespace FeatherOS::VFS

#endif
