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
constexpr uint32_t ReadOnlyFileMode = User::ModeRegular | 0444;
constexpr uint32_t ReadOnlyDirectoryMode = User::ModeDirectory | 0555;
constexpr uint32_t WritableFileMode = User::ModeRegular | 0644;
constexpr uint32_t WritableDirectoryMode = User::ModeDirectory | 0755;
constexpr uint32_t DeviceMode = User::ModeCharacterDevice | 0666;
constexpr uint32_t LinkMode = User::ModeSymbolicLink | 0777;
// The number every in-memory filesystem gives its root.
constexpr uint64_t RootSerialNumber = 1;

// The virtual filesystems have no owners, times or hard links.
FileInfo MakeInfo(uint64_t node, uint32_t mode, uint64_t size)
{
    FileInfo info = {};
    info.node = node;
    info.mode = mode;
    info.size = size;
    info.link_count = 1;
    return info;
}

struct GeneratedText
{
    char text[Procfs::MaximumFileSize];
};

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
        if (*path == '\0')
        {
            *info = MakeInfo(RootSerialNumber, ReadOnlyDirectoryMode, 0);
            return true;
        }
        Initramfs::File file;
        if (!Initramfs::Open(directory, path, &file))
        {
            return false;
        }
        *info = MakeInfo(MemoryId(file.data), ReadOnlyFileMode, file.size);
        return true;
    }

    bool StatNode(const Node& node, FileInfo* info) override
    {
        *info = MakeInfo(node.id, ReadOnlyFileMode, 0);
        return true;
    }

    bool Read(const Node& node, uint64_t size, uint64_t offset, void* buffer, uint64_t length,
              uint64_t* bytes_read) override
    {
        return CopyFromMemory(node, size, offset, buffer, length, bytes_read);
    }

    bool ReadDirectory(const char* path, uint32_t index, DirectoryEntry* entry) override
    {
        Initramfs::File file;
        if (*path != '\0' || !Initramfs::ReadDirectory(directory, index, entry->name, &file))
        {
            return false;
        }
        entry->type = User::ModeRegular;
        entry->node = MemoryId(file.data);
        entry->size = file.size;
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
        if (FeatherOS::Devices::IsDirectory(path))
        {
            *info =
                MakeInfo(FeatherOS::Devices::DirectorySerialNumber(path), WritableDirectoryMode, 0);
            return true;
        }
        uint32_t device;
        if (!FeatherOS::Devices::Find(path, &device))
        {
            return false;
        }
        *info = MakeInfo(FeatherOS::Devices::SerialNumber(device), DeviceMode, 0);
        return true;
    }

    bool StatNode(const Node& node, FileInfo* info) override
    {
        *info = MakeInfo(FeatherOS::Devices::SerialNumber(static_cast<uint32_t>(node.id)),
                         DeviceMode, 0);
        return true;
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

    // The payload is the file's number for stat.
    bool Open(const char* path, Node* node, uint64_t* size) override
    {
        uint64_t serial_number = 0;
        bool directory = false;
        if (!Procfs::Find(path, &serial_number, &directory) || directory)
        {
            return false;
        }
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
        *node = {MemoryId(text), serial_number};
        *size = length;
        return true;
    }

    bool Stat(const char* path, FileInfo* info) override
    {
        uint64_t serial_number = 0;
        bool directory = false;
        if (!Procfs::Find(path, &serial_number, &directory))
        {
            return false;
        }
        if (directory)
        {
            *info = MakeInfo(serial_number, ReadOnlyDirectoryMode, 0);
            return true;
        }
        const Memory::HeapObject<GeneratedText> generated;
        size_t length = 0;
        if (!generated || !Procfs::Generate(path, generated->text, &length))
        {
            return false;
        }
        *info = MakeInfo(serial_number, ReadOnlyFileMode, length);
        return true;
    }

    bool StatNode(const Node& node, FileInfo* info) override
    {
        *info = MakeInfo(node.payload, ReadOnlyFileMode, 0);
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
        if (!Tmpfs::Find(root, path, &index))
        {
            return false;
        }
        const uint32_t mode = Tmpfs::IsDirectoryNode(index) ? WritableDirectoryMode
                              : Tmpfs::IsLinkNode(index)    ? LinkMode
                                                            : WritableFileMode;
        *info = MakeInfo(Tmpfs::SerialNumber(index), mode, Tmpfs::Size(index));
        return true;
    }

    bool StatNode(const Node& node, FileInfo* info) override
    {
        *info = MakeInfo(Tmpfs::SerialNumber(Index(node)), WritableFileMode, 0);
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

    bool SupportsLinks(LinkKind kind) override
    {
        return kind == LinkKind::Symbolic;
    }

    bool FindLink(const char* path, uint32_t* link_end) override
    {
        return Tmpfs::FindLink(root, path, link_end);
    }

    bool ReadLink(const char* path, char* buffer, uint32_t capacity, uint32_t* length) override
    {
        uint32_t index;
        return Tmpfs::Find(root, path, &index) && Tmpfs::ReadLink(index, buffer, capacity, length);
    }

    bool SymbolicLink(const char* path, const char* target) override
    {
        uint32_t index;
        return Tmpfs::SymbolicLink(root, path, target, &index);
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

constexpr CoreKind Kinds[] = {
    {FeatherDisk::Fat::Fat16Type, "fat16", {}},
    {FeatherDisk::Ext::Ext2Type, "ext2", {.periodic_sync = true}},
    {FeatherDisk::Ext::Ext3Type, "ext3", {.periodic_sync = true, .close_on_detach = true}},
    {FeatherDisk::Ext::Ext4Type, "ext4", {.periodic_sync = true, .close_on_detach = true}},
};

constinit CoreDriver volume_drivers[Drivers::MaximumVolumes];
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

CoreDriver* Volume(size_t number)
{
    return number >= 1 && number <= MaximumVolumes ? &volume_drivers[number - 1] : nullptr;
}

CoreDriver* VolumeOf(const Driver* driver)
{
    for (CoreDriver& volume : volume_drivers)
    {
        if (&volume == driver)
        {
            return &volume;
        }
    }
    return nullptr;
}

size_t FindPartitions(std::span<FeatherDisk::Partition::Range> ranges)
{
    return FeatherDisk::Partition::Find(BlockDevice::CoreDevice(), ranges);
}

bool OpenVolume(const FeatherDisk::Partition::Range& range)
{
    CoreDriver* const volume = Volume(range.number);
    return volume != nullptr && volume->OpenPartition(Kinds, BlockDevice::CoreDevice(), range);
}

bool ForgetVolume(size_t number)
{
    CoreDriver* const volume = Volume(number);
    return volume != nullptr && volume->Forget();
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
