#ifndef FEATHEROS_FILESYSTEM_INITRAMFS_H
#define FEATHEROS_FILESYSTEM_INITRAMFS_H

#include <stdint.h>

namespace FeatherOS::Initramfs
{

struct File
{
    const uint8_t* data;
    uint64_t size;
};

bool Mount(const void* archive, uint64_t size);
bool Open(const char* path, File* file);
// The index-th file of the archive; false past the last one.
bool ReadDirectory(uint32_t index, char (&name)[64], uint64_t* size);

} // namespace FeatherOS::Initramfs

#endif