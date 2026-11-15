#include "drivers.h"

#include "devices.h"
#include "initramfs.h"
#include "procfs.h"
#include "tmpfs.h"

#include "fs/ext2.h"
#include "fs/fat.h"
#include "kernel/memory/heap.h"
#include "kernel/storage/core_block_device.h"

#include <stdint.h>

namespace FeatherOS::Filesystem
{
namespace
{

constexpr Properties Writable = {.read_only = false, .tracked = true, .devices = false};
constexpr Properties ReadOnly = {.read_only = true, .tracked = false, .devices = false};
constexpr Properties DeviceFiles = {.read_only = false, .tracked = false, .devices = true};

// Archive and generated files are bytes in memory; their node ID is the address.
uint64_t MemoryId(const void* data)
{
    return reinterpret_cast<uintptr_t>(data);
}

const uint8_t* MemoryData(const Node& node)
{
    return reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(node.id));
}

bool CopyFromMemory(const Node& node, uint64_t size, uint64_t offset, void* buffer, uint64_t length,
                    uint64_t* bytes_read)
{
    const uint8_t* data = MemoryData(node);
    const uint64_t available = size - offset;
    const uint64_t count = length < available ? length : available;
    auto* destination = static_cast<uint8_t*>(buffer);
    for (uint64_t index = 0; index < count; ++index)
    {
        destination[index] = data[offset + index];
    }
    *bytes_read = count;
    return true;
}

// One top-level directory of the initramfs.
class ArchiveDriver final : public Driver
{
public:
    constexpr explicit ArchiveDriver(const char* directory) : Driver(ReadOnly), directory(directory)
    {
    }

    bool Open(const char* path, Node* node, uint64_t* size) override
    {
        Initramfs::File file;
        if (!Initramfs::Open(directory, path, &file))
        {
            return false;
        }
        *node = {MemoryId(file.data), 0};
        *size = file.size;
        return true;
    }

    bool Stat(const char* path, FileInfo* info) override
    {
        Initramfs::File file;
        if (!Initramfs::Open(directory, path, &file))
        {
            return false;
        }
        info->size = file.size;
        return true;
    }

    bool Read(const Node& node, uint64_t size, uint64_t offset, void* buffer, uint64_t length,
              uint64_t* bytes_read) override
    {
        return CopyFromMemory(node, size, offset, buffer, length, bytes_read);
    }

    bool ReadDirectory(const char* path, uint32_t index, DirectoryEntry* entry) override
    {
        uint64_t size;
        if (*path != '\0' || !Initramfs::ReadDirectory(directory, index, entry->name, &size))
        {
            return false;
        }
        entry->directory = false;
        entry->size = static_cast<uint32_t>(size);
        return true;
    }

private:
    const char* directory;
};

// A device's node ID is the node Devices::Find gives.
class DeviceDriver final : public Driver
{
public:
    constexpr DeviceDriver() : Driver(DeviceFiles) {}

    bool Open(const char* path, Node* node, uint64_t* size) override
    {
        uint32_t device;
        if (!FeatherOS::Devices::Find(path, &device))
        {
            return false;
        }
        *node = {device, 0};
        *size = 0;
        return true;
    }

    bool Stat(const char* path, FileInfo* info) override
    {
        uint32_t device;
        info->size = 0;
        info->device = true;
        return FeatherOS::Devices::Find(path, &device);
    }

    bool Read(const Node& node, uint64_t /*size*/, uint64_t /*offset*/, void* buffer,
              uint64_t length, uint64_t* bytes_read) override
    {
        FeatherOS::Devices::Read(FeatherOS::Devices::DeviceOf(static_cast<uint32_t>(node.id)),
                                 buffer, length, bytes_read);
        return true;
    }

    bool Write(Node* /*node*/, uint64_t* /*size*/, uint64_t /*offset*/, const void* /*buffer*/,
               uint64_t length, uint64_t* bytes_written) override
    {
        *bytes_written = length;
        return true;
    }

    bool Truncate(Node* /*node*/, uint64_t* /*size*/, uint64_t /*new_size*/) override
    {
        return true;
    }

    bool IsDirectory(const char* path) override
    {
        return FeatherOS::Devices::IsDirectory(path);
    }

    bool ReadDirectory(const char* path, uint32_t index, DirectoryEntry* entry) override
    {
        return FeatherOS::Devices::ReadDirectory(path, index, entry);
    }

    bool IsReadOnly(const char* path) override
    {
        uint32_t device;
        return !FeatherOS::Devices::Find(path, &device);
    }
};

// A generated file's text is made at open and owned by the handle.
class ProcDriver final : public Driver
{
public:
    constexpr ProcDriver() : Driver(ReadOnly) {}

    bool Open(const char* path, Node* node, uint64_t* size) override
    {
        auto* text = static_cast<char*>(Memory::Allocate(Procfs::MaximumFileSize));
        size_t length = 0;
        if (text == nullptr)
        {
            return false;
        }
        if (!Procfs::Generate(path, text, &length))
        {
            Memory::Deallocate(text);
            return false;
        }
        *node = {MemoryId(text), 0};
        *size = length;
        return true;
    }

    bool Stat(const char* path, FileInfo* info) override
    {
        char text[Procfs::MaximumFileSize];
        size_t length = 0;
        if (!Procfs::Generate(path, text, &length))
        {
            return false;
        }
        info->size = length;
        return true;
    }

    bool Read(const Node& node, uint64_t size, uint64_t offset, void* buffer, uint64_t length,
              uint64_t* bytes_read) override
    {
        return CopyFromMemory(node, size, offset, buffer, length, bytes_read);
    }

    void Close(const Node& node) override
    {
        if (node.id != 0)
        {
            Memory::Deallocate(const_cast<uint8_t*>(MemoryData(node)));
        }
    }

    bool IsDirectory(const char* path) override
    {
        return Procfs::IsDirectory(path);
    }

    bool ReadDirectory(const char* path, uint32_t index, DirectoryEntry* entry) override
    {
        return Procfs::ReadDirectory(path, index, entry);
    }
};

// A tmpfs node's ID is its node index.
class MemoryDriver final : public Driver
{
public:
    constexpr explicit MemoryDriver(Tmpfs::Root root) : Driver(Writable), root(root) {}

    bool Open(const char* path, Node* node, uint64_t* size) override
    {
        uint32_t index;
        if (!Tmpfs::Open(root, path, &index))
        {
            return false;
        }
        *node = {index, 0};
        *size = Tmpfs::Size(index);
        return true;
    }

    bool Stat(const char* path, FileInfo* info) override
    {
        uint32_t index;
        if (!Tmpfs::Open(root, path, &index))
        {
            return false;
        }
        info->size = Tmpfs::Size(index);
        return true;
    }

    bool Read(const Node& node, uint64_t /*size*/, uint64_t offset, void* buffer, uint64_t length,
              uint64_t* bytes_read) override
    {
        return Tmpfs::Read(Index(node), offset, buffer, length, bytes_read);
    }

    bool Create(const char* path, Node* node, uint64_t* size) override
    {
        uint32_t index;
        if (!Tmpfs::Create(root, path, &index))
        {
            return false;
        }
        *node = {index, 0};
        *size = 0;
        return true;
    }

    bool Write(Node* node, uint64_t* size, uint64_t offset, const void* buffer, uint64_t length,
               uint64_t* bytes_written) override
    {
        const bool written = Tmpfs::Write(Index(*node), offset, buffer, length, bytes_written);
        *size = Tmpfs::Size(Index(*node));
        return written;
    }

    bool Truncate(Node* node, uint64_t* size, uint64_t new_size) override
    {
        const bool truncated = Tmpfs::Truncate(Index(*node), new_size);
        *size = Tmpfs::Size(Index(*node));
        return truncated;
    }

    bool Remove(const char* path) override
    {
        return Tmpfs::Remove(root, path);
    }

    bool MakeDirectory(const char* path) override
    {
        return Tmpfs::MakeDirectory(root, path);
    }

    bool RemoveDirectory(const char* path) override
    {
        return Tmpfs::RemoveDirectory(root, path);
    }

    bool Rename(const char* from, const char* to) override
    {
        return Tmpfs::Rename(root, from, to);
    }

    bool IsDirectory(const char* path) override
    {
        return Tmpfs::IsDirectory(root, path);
    }

    bool ReadDirectory(const char* path, uint32_t index, DirectoryEntry* entry) override
    {
        return Tmpfs::ReadDirectory(root, path, index, entry);
    }

private:
    static uint32_t Index(const Node& node)
    {
        return static_cast<uint32_t>(node.id);
    }

    Tmpfs::Root root;
};

constexpr const char* PartitionSources[] = {
    "vda",  "vda1",  "vda2",  "vda3",  "vda4",  "vda5",  "vda6",  "vda7",  "vda8",
    "vda9", "vda10", "vda11", "vda12", "vda13", "vda14", "vda15", "vda16",
};

constinit CoreDriver disk_driver(Writable);
constinit CoreDriver ext2_driver(ReadOnly);
constinit ArchiveDriver system_driver("system");
constinit DeviceDriver device_driver;
constinit ProcDriver proc_driver;
constinit MemoryDriver tmp_driver(Tmpfs::Root::Tmp);
constinit MemoryDriver etc_driver(Tmpfs::Root::Etc);

} // namespace

namespace Drivers
{

const char* PartitionSource(size_t number)
{
    return number < sizeof(PartitionSources) / sizeof(PartitionSources[0])
               ? PartitionSources[number]
               : nullptr;
}

CoreDriver& Disk()
{
    return disk_driver;
}

bool MountDisk()
{
    return disk_driver.Mount(FeatherDisk::Fat::Fat16Type(), BlockDevice::CoreDevice());
}

CoreDriver& Ext2()
{
    return ext2_driver;
}

bool MountExt2()
{
    return ext2_driver.Mount(FeatherDisk::Ext::Ext2Type(), BlockDevice::CoreDevice());
}

Driver& System()
{
    return system_driver;
}

Driver& Devices()
{
    return device_driver;
}

Driver& Proc()
{
    return proc_driver;
}

Driver& Tmp()
{
    return tmp_driver;
}

Driver& Etc()
{
    return etc_driver;
}

} // namespace Drivers

} // namespace FeatherOS::Filesystem
