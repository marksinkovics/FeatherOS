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

} // namespace FeatherOS::Initramfs

#endif