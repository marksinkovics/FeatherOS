#ifndef FEATHEROS_FILESYSTEM_VFS_H
#define FEATHEROS_FILESYSTEM_VFS_H

#include "kernel/filesystem/fat16.h"

#include <stdint.h>

namespace FeatherOS::VFS
{

struct FileHandle
{
    const uint8_t* data;
    uint64_t size;
    uint64_t offset;
    uint32_t first_cluster;
    uint32_t directory_cluster;
    uint32_t entry_index;
    bool open;
    bool block_backed;
};

struct FileInfo
{
    uint64_t size;
};

bool MountBlockDevice();
bool Open(const char* path, FileHandle* file);
bool Read(FileHandle* file, void* buffer, uint64_t length, uint64_t* bytes_read);
bool Seek(FileHandle* file, uint64_t offset);
bool Stat(const char* path, FileInfo* info);
void Close(FileHandle* file);
// Only disk files can be changed; the initramfs under /system is read-only. A handle caches the
// file's size, so other handles to the same file do not see its changes.
bool Create(const char* path, FileHandle* file);
// Writes at the handle's offset and advances it; *bytes_written may be short if the disk fills.
bool Write(FileHandle* file, const void* buffer, uint64_t length, uint64_t* bytes_written);
bool Truncate(FileHandle* file, uint64_t size);
bool Remove(const char* path);
bool MakeDirectory(const char* path);
bool IsDirectory(const char* path);
// True for the initramfs: /system, the paths under it, and the bare names kernel code uses.
bool IsReadOnly(const char* path);
bool RemoveDirectory(const char* path);
// Lists a directory, such as "/", "/home/docs", or "/system"; "/" starts with the system entry.
// False past the last entry.
bool ReadDirectory(const char* path, uint32_t index, FAT16::DirectoryEntry* entry);

} // namespace FeatherOS::VFS

#endif