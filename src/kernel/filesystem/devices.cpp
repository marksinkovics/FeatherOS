#include "devices.h"

#include "kernel/random.h"

namespace FeatherOS::Devices
{
namespace
{

struct Entry
{
    const char* name;
    Device device;
};

constexpr Entry Entries[] = {
    {"null", Device::Null},
    {"zero", Device::Zero},
    {"random", Device::Random},
};

bool Equals(const char* left, const char* right)
{
    while (*left != '\0' && *left == *right)
    {
        ++left;
        ++right;
    }
    return *left == *right;
}

} // namespace

bool Find(const char* path, Device* device)
{
    for (const Entry& entry : Entries)
    {
        if (Equals(path, entry.name))
        {
            *device = entry.device;
            return true;
        }
    }
    return false;
}

void Read(Device device, void* buffer, uint64_t length, uint64_t* bytes_read)
{
    auto* bytes = static_cast<uint8_t*>(buffer);
    if (device == Device::Null)
    {
        *bytes_read = 0;
        return;
    }
    uint64_t random = 0;
    for (uint64_t index = 0; index < length; ++index)
    {
        if (device == Device::Zero)
        {
            bytes[index] = 0;
            continue;
        }
        if (index % sizeof(random) == 0)
        {
            random = Random::Next();
        }
        bytes[index] = static_cast<uint8_t>(random >> (8 * (index % sizeof(random))));
    }
    *bytes_read = length;
}

bool ReadDirectory(uint32_t index, FAT16::DirectoryEntry* entry)
{
    if (index >= sizeof(Entries) / sizeof(Entries[0]))
    {
        return false;
    }
    uint32_t length = 0;
    for (const char* name = Entries[index].name; name[length] != '\0'; ++length)
    {
        entry->name[length] = name[length];
    }
    entry->name[length] = '\0';
    entry->directory = false;
    entry->size = 0;
    return true;
}

} // namespace FeatherOS::Devices
