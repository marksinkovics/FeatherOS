#include "vfs.h"

#include "devices.h"
#include "fat16.h"
#include "initramfs.h"
#include "procfs.h"
#include "tmpfs.h"

#include "kernel/memory/heap.h"

namespace FeatherOS::VFS
{
namespace
{

enum class Mount : uint8_t
{
    Disk,
    System,
    Devices,
    Proc,
    Tmp,
};

struct MountPoint
{
    const char* name;
    Mount mount;
};

// In the order "/" lists them.
constexpr MountPoint MountPoints[] = {
    {"system", Mount::System},
    {"dev", Mount::Devices},
    {"proc", Mount::Proc},
    {"tmp", Mount::Tmp},
};
constexpr uint32_t MountPointCount = sizeof(MountPoints) / sizeof(MountPoints[0]);

// Which filesystem holds path, and the path inside it ("" for its root). The bare names kernel
// code uses ("init") name the initramfs.
Mount Locate(const char* path, const char** inner)
{
    if (path[0] != '/')
    {
        *inner = path;
        return Mount::System;
    }
    for (const MountPoint& point : MountPoints)
    {
        uint32_t length = 0;
        while (point.name[length] != '\0' && path[1 + length] == point.name[length])
        {
            ++length;
        }
        const char next = path[1 + length];
        if (point.name[length] == '\0' && (next == '\0' || next == '/'))
        {
            const char* rest = path + 1 + length;
            while (*rest == '/')
            {
                ++rest;
            }
            *inner = rest;
            return point.mount;
        }
    }
    *inner = path + 1;
    return Mount::Disk;
}

void ResetHandle(FileHandle* file, FileKind kind, uint64_t size)
{
    *file = {};
    file->kind = kind;
    file->size = size;
    file->open = true;
}

void OpenDiskHandle(const FAT16::File& disk_file, FileHandle* file)
{
    ResetHandle(file, FileKind::Disk, disk_file.size);
    file->first_cluster = disk_file.first_cluster;
    file->directory_cluster = disk_file.directory_cluster;
    file->entry_index = disk_file.entry_index;
}

FAT16::File DiskFile(const FileHandle& file)
{
    return {file.first_cluster, static_cast<uint32_t>(file.size), file.directory_cluster,
            file.entry_index};
}

void UpdateHandle(const FAT16::File& disk_file, FileHandle* file)
{
    file->size = disk_file.size;
    file->first_cluster = disk_file.first_cluster;
}

void CopyFromMemory(FileHandle* file, void* buffer, uint64_t length, uint64_t* bytes_read)
{
    const uint64_t available = file->size - file->offset;
    const uint64_t count = length < available ? length : available;
    auto* destination = static_cast<uint8_t*>(buffer);
    for (uint64_t index = 0; index < count; ++index)
    {
        destination[index] = file->data[file->offset + index];
    }
    file->offset += count;
    *bytes_read = count;
}

void SetEntry(FAT16::DirectoryEntry* entry, const char* name)
{
    uint32_t length = 0;
    for (; name[length] != '\0'; ++length)
    {
        entry->name[length] = name[length];
    }
    entry->name[length] = '\0';
    entry->directory = true;
    entry->size = 0;
}

} // namespace

bool MountBlockDevice()
{
    return FAT16::Mount();
}

bool Open(const char* path, FileHandle* file)
{
    if (path == nullptr || file == nullptr)
    {
        return false;
    }
    const char* inner;
    switch (Locate(path, &inner))
    {
    case Mount::Disk:
    {
        FAT16::File disk_file;
        if (*inner == '\0' || !FAT16::Open(inner, &disk_file))
        {
            return false;
        }
        OpenDiskHandle(disk_file, file);
        return true;
    }
    case Mount::System:
    {
        Initramfs::File archive_file;
        if (!Initramfs::Open(inner, &archive_file))
        {
            return false;
        }
        ResetHandle(file, FileKind::Archive, archive_file.size);
        file->data = archive_file.data;
        return true;
    }
    case Mount::Devices:
    {
        Devices::Device device;
        if (!Devices::Find(inner, &device))
        {
            return false;
        }
        ResetHandle(file, FileKind::Device, 0);
        file->node = static_cast<uint32_t>(device);
        return true;
    }
    case Mount::Proc:
    {
        auto* text = static_cast<char*>(Memory::Allocate(Procfs::MaximumFileSize));
        size_t length = 0;
        if (text == nullptr)
        {
            return false;
        }
        if (!Procfs::Generate(inner, text, &length))
        {
            Memory::Deallocate(text);
            return false;
        }
        ResetHandle(file, FileKind::Generated, length);
        file->data = reinterpret_cast<const uint8_t*>(text);
        return true;
    }
    case Mount::Tmp:
    {
        uint32_t node;
        if (!Tmpfs::Open(inner, &node))
        {
            return false;
        }
        ResetHandle(file, FileKind::Memory, Tmpfs::Size(node));
        file->node = node;
        return true;
    }
    }
    return false;
}

bool Read(FileHandle* file, void* buffer, uint64_t length, uint64_t* bytes_read)
{
    if (bytes_read == nullptr)
    {
        return false;
    }
    *bytes_read = 0;
    if (file == nullptr || !file->open || (length > 0 && buffer == nullptr) ||
        file->offset > file->size)
    {
        return false;
    }
    switch (file->kind)
    {
    case FileKind::Disk:
    {
        const FAT16::File disk_file = DiskFile(*file);
        if (!FAT16::Read(&disk_file, file->offset, buffer, length, bytes_read))
        {
            return false;
        }
        file->offset += *bytes_read;
        return true;
    }
    case FileKind::Device:
        Devices::Read(static_cast<Devices::Device>(file->node), buffer, length, bytes_read);
        return true;
    case FileKind::Memory:
        if (!Tmpfs::Read(file->node, file->offset, buffer, length, bytes_read))
        {
            return false;
        }
        file->offset += *bytes_read;
        return true;
    case FileKind::Archive:
    case FileKind::Generated:
        CopyFromMemory(file, buffer, length, bytes_read);
        return true;
    }
    return false;
}

bool Seek(FileHandle* file, uint64_t offset)
{
    if (file == nullptr || !file->open || (file->kind != FileKind::Device && offset > file->size))
    {
        return false;
    }
    file->offset = file->kind == FileKind::Device ? 0 : offset;
    return true;
}

bool Stat(const char* path, FileInfo* info)
{
    if (path == nullptr || info == nullptr)
    {
        return false;
    }
    info->device = false;
    const char* inner;
    switch (Locate(path, &inner))
    {
    case Mount::Disk:
    {
        FAT16::FileInfo disk_file_info;
        if (*inner == '\0' || !FAT16::Stat(inner, &disk_file_info))
        {
            return false;
        }
        info->size = disk_file_info.size;
        return true;
    }
    case Mount::System:
    {
        Initramfs::File archive_file;
        if (!Initramfs::Open(inner, &archive_file))
        {
            return false;
        }
        info->size = archive_file.size;
        return true;
    }
    case Mount::Devices:
    {
        Devices::Device device;
        info->size = 0;
        info->device = true;
        return Devices::Find(inner, &device);
    }
    case Mount::Proc:
    {
        char text[Procfs::MaximumFileSize];
        size_t length = 0;
        if (!Procfs::Generate(inner, text, &length))
        {
            return false;
        }
        info->size = length;
        return true;
    }
    case Mount::Tmp:
    {
        uint32_t node;
        if (!Tmpfs::Open(inner, &node))
        {
            return false;
        }
        info->size = Tmpfs::Size(node);
        return true;
    }
    }
    return false;
}

void Close(FileHandle* file)
{
    if (file == nullptr)
    {
        return;
    }
    if (file->open && file->kind == FileKind::Generated && file->data != nullptr)
    {
        Memory::Deallocate(const_cast<uint8_t*>(file->data));
    }
    *file = {};
}

bool Create(const char* path, FileHandle* file)
{
    if (path == nullptr || file == nullptr)
    {
        return false;
    }
    const char* inner;
    const Mount mount = Locate(path, &inner);
    if (*inner == '\0')
    {
        return false;
    }
    if (mount == Mount::Disk)
    {
        FAT16::File disk_file;
        if (!FAT16::Create(inner, &disk_file))
        {
            return false;
        }
        OpenDiskHandle(disk_file, file);
        return true;
    }
    uint32_t node;
    if (mount != Mount::Tmp || !Tmpfs::Create(inner, &node))
    {
        return false;
    }
    ResetHandle(file, FileKind::Memory, 0);
    file->node = node;
    return true;
}

bool Write(FileHandle* file, const void* buffer, uint64_t length, uint64_t* bytes_written)
{
    if (bytes_written == nullptr)
    {
        return false;
    }
    *bytes_written = 0;
    if (file == nullptr || !file->open)
    {
        return false;
    }
    switch (file->kind)
    {
    case FileKind::Disk:
    {
        FAT16::File disk_file = DiskFile(*file);
        const bool written = FAT16::Write(&disk_file, file->offset, buffer, length, bytes_written);
        UpdateHandle(disk_file, file);
        file->offset += *bytes_written;
        return written;
    }
    case FileKind::Memory:
    {
        const bool written = Tmpfs::Write(file->node, file->offset, buffer, length, bytes_written);
        file->size = Tmpfs::Size(file->node);
        file->offset += *bytes_written;
        return written;
    }
    case FileKind::Device:
        *bytes_written = length;
        return true;
    case FileKind::Archive:
    case FileKind::Generated:
        return false;
    }
    return false;
}

bool Truncate(FileHandle* file, uint64_t size)
{
    if (file == nullptr || !file->open)
    {
        return false;
    }
    bool truncated = false;
    switch (file->kind)
    {
    case FileKind::Disk:
    {
        FAT16::File disk_file = DiskFile(*file);
        truncated = FAT16::Truncate(&disk_file, size);
        UpdateHandle(disk_file, file);
        break;
    }
    case FileKind::Memory:
        truncated = Tmpfs::Truncate(file->node, size);
        file->size = Tmpfs::Size(file->node);
        break;
    case FileKind::Device:
        return true;
    case FileKind::Archive:
    case FileKind::Generated:
        return false;
    }
    if (file->offset > file->size)
    {
        file->offset = file->size;
    }
    return truncated;
}

bool ReadDirectory(const char* path, uint32_t index, FAT16::DirectoryEntry* entry)
{
    if (path == nullptr || entry == nullptr)
    {
        return false;
    }
    const char* inner;
    switch (Locate(path, &inner))
    {
    case Mount::Disk:
        if (*inner != '\0')
        {
            return FAT16::ReadDirectory(inner, index, entry);
        }
        // The root lists the mount points, then the disk's own root entries.
        if (index < MountPointCount)
        {
            SetEntry(entry, MountPoints[index].name);
            return true;
        }
        return FAT16::ReadDirectory("", index - MountPointCount, entry);
    case Mount::System:
    {
        uint64_t size;
        if (path[0] != '/' || *inner != '\0' ||
            !Initramfs::ReadDirectory(index, entry->name, &size))
        {
            return false;
        }
        entry->directory = false;
        entry->size = static_cast<uint32_t>(size);
        return true;
    }
    case Mount::Devices:
        return *inner == '\0' && Devices::ReadDirectory(index, entry);
    case Mount::Proc:
        return Procfs::ReadDirectory(inner, index, entry);
    case Mount::Tmp:
        return Tmpfs::ReadDirectory(inner, index, entry);
    }
    return false;
}

bool IsDirectory(const char* path)
{
    if (path == nullptr)
    {
        return false;
    }
    const char* inner;
    switch (Locate(path, &inner))
    {
    case Mount::Disk:
        // The root exists even without a disk, since it holds the mount points.
        return *inner == '\0' || FAT16::IsDirectory(inner);
    case Mount::System:
    case Mount::Devices:
        return path[0] == '/' && *inner == '\0';
    case Mount::Proc:
        return Procfs::IsDirectory(inner);
    case Mount::Tmp:
        return Tmpfs::IsDirectory(inner);
    }
    return false;
}

bool IsReadOnly(const char* path)
{
    if (path == nullptr)
    {
        return false;
    }
    const char* inner;
    Devices::Device device;
    switch (Locate(path, &inner))
    {
    case Mount::System:
    case Mount::Proc:
        return true;
    case Mount::Devices:
        return !Devices::Find(inner, &device);
    case Mount::Disk:
    case Mount::Tmp:
        return false;
    }
    return false;
}

bool IsMountPoint(const char* path)
{
    if (path == nullptr || path[0] != '/')
    {
        return false;
    }
    const char* inner;
    Locate(path, &inner);
    return *inner == '\0';
}

bool IsTracked(const FileHandle& file)
{
    return file.kind == FileKind::Disk || file.kind == FileKind::Memory;
}

bool MakeDirectory(const char* path)
{
    if (path == nullptr)
    {
        return false;
    }
    const char* inner;
    const Mount mount = Locate(path, &inner);
    if (*inner == '\0')
    {
        return false;
    }
    return mount == Mount::Disk  ? FAT16::MakeDirectory(inner)
           : mount == Mount::Tmp ? Tmpfs::MakeDirectory(inner)
                                 : false;
}

bool RemoveDirectory(const char* path)
{
    if (path == nullptr)
    {
        return false;
    }
    const char* inner;
    const Mount mount = Locate(path, &inner);
    if (*inner == '\0')
    {
        return false;
    }
    return mount == Mount::Disk  ? FAT16::RemoveDirectory(inner)
           : mount == Mount::Tmp ? Tmpfs::RemoveDirectory(inner)
                                 : false;
}

bool Remove(const char* path)
{
    if (path == nullptr)
    {
        return false;
    }
    const char* inner;
    const Mount mount = Locate(path, &inner);
    if (*inner == '\0')
    {
        return false;
    }
    return mount == Mount::Disk  ? FAT16::Remove(inner)
           : mount == Mount::Tmp ? Tmpfs::Remove(inner)
                                 : false;
}

bool SameFilesystem(const char* left, const char* right)
{
    if (left == nullptr || right == nullptr)
    {
        return false;
    }
    const char* inner;
    return Locate(left, &inner) == Locate(right, &inner);
}

bool Rename(const char* from, const char* to)
{
    if (from == nullptr || to == nullptr)
    {
        return false;
    }
    const char* from_inner;
    const char* to_inner;
    const Mount mount = Locate(from, &from_inner);
    if (Locate(to, &to_inner) != mount || *from_inner == '\0' || *to_inner == '\0')
    {
        return false;
    }
    return mount == Mount::Disk  ? FAT16::Rename(from_inner, to_inner)
           : mount == Mount::Tmp ? Tmpfs::Rename(from_inner, to_inner)
                                 : false;
}

} // namespace FeatherOS::VFS
