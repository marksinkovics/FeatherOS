#ifndef FEATHEROS_FILESYSTEM_VFS_H
#define FEATHEROS_FILESYSTEM_VFS_H

#include <stdint.h>

namespace FeatherOS::VFS
{

struct FileHandle
{
    const uint8_t* data;
    uint64_t size;
    uint64_t offset;
    uint32_t first_cluster;
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

} // namespace FeatherOS::VFS

#endif