#ifndef FEATHEROS_FILESYSTEM_FAT16_H
#define FEATHEROS_FILESYSTEM_FAT16_H

#include <stdint.h>

namespace FeatherOS::FAT16
{

struct File
{
    uint32_t first_cluster;
    uint32_t size;
};

struct FileInfo
{
    uint32_t size;
};

bool Mount();
bool Open(const char* path, File* file);
bool Stat(const char* path, FileInfo* info);
bool Read(const File* file, uint64_t offset, void* buffer, uint64_t length, uint64_t* bytes_read);

} // namespace FeatherOS::FAT16

#endif