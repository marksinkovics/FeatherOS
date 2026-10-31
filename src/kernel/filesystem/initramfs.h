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
// The archive's top-level directories are mount points, such as "system" for /system.
bool Open(const char* directory, const char* name, File* file);
// The index-th file directly inside directory; false past the last one.
bool ReadDirectory(const char* directory, uint32_t index, char (&name)[64], uint64_t* size);

} // namespace FeatherOS::Initramfs

#endif