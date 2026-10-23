#ifndef FEATHEROS_FILESYSTEM_DEVICES_H
#define FEATHEROS_FILESYSTEM_DEVICES_H

#include "kernel/filesystem/fat16.h"

#include <stdint.h>

// The character devices mounted at /dev. Every device accepts and discards writes, and has no
// size, so seeking does nothing.
namespace FeatherOS::Devices
{

enum class Device : uint8_t
{
    // Reads end at once.
    Null,
    // Reads return zero bytes.
    Zero,
    // Reads return random bytes, from RDRAND or RNDR when the CPU has it.
    Random,
};

// path is relative to the mount, such as "null".
bool Find(const char* path, Device* device);
void Read(Device device, void* buffer, uint64_t length, uint64_t* bytes_read);
bool ReadDirectory(uint32_t index, FAT16::DirectoryEntry* entry);

} // namespace FeatherOS::Devices

#endif
