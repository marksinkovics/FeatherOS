#include "vfs.h"

#include "fat16.h"
#include "initramfs.h"

namespace FeatherOS::VFS
{
namespace
{

constexpr char SystemMountPath[] = "/system";

// "/system" and the paths under it name the read-only initramfs, as do the bare names kernel code
// uses. Every other absolute path names the disk, which is mounted at "/".
bool IsSystemPath(const char* path)
{
    if (path[0] != '/')
    {
        return true;
    }
    for (uint32_t index = 0; index < sizeof(SystemMountPath) - 1; ++index)
    {
        if (path[index] != SystemMountPath[index])
        {
            return false;
        }
    }
    const char next = path[sizeof(SystemMountPath) - 1];
    return next == '\0' || next == '/';
}

// The initramfs name: "/system/hello.txt" is its "hello.txt", and "" is the mount point itself.
const char* ArchivePath(const char* path)
{
    if (path[0] != '/')
    {
        return path;
    }
    const char* rest = path + sizeof(SystemMountPath) - 1;
    while (*rest == '/')
    {
        ++rest;
    }
    return rest;
}

// The path inside the disk volume ("" for "/"), or nullptr for an initramfs path.
const char* VolumePath(const char* path)
{
    return IsSystemPath(path) ? nullptr : path + 1;
}

// A disk path that names an entry rather than the volume's root.
const char* DiskEntryPath(const char* path)
{
    const char* volume_path = VolumePath(path);
    return volume_path != nullptr && *volume_path != '\0' ? volume_path : nullptr;
}

void OpenDiskHandle(const FAT16::File& disk_file, FileHandle* file)
{
    file->data = nullptr;
    file->size = disk_file.size;
    file->offset = 0;
    file->first_cluster = disk_file.first_cluster;
    file->directory_cluster = disk_file.directory_cluster;
    file->entry_index = disk_file.entry_index;
    file->open = true;
    file->block_backed = true;
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

    if (const char* volume_path = DiskEntryPath(path))
    {
        FAT16::File disk_file;
        if (!FAT16::Open(volume_path, &disk_file))
        {
            return false;
        }
        OpenDiskHandle(disk_file, file);
        return true;
    }

    Initramfs::File archive_file;
    if (!Initramfs::Open(ArchivePath(path), &archive_file))
    {
        return false;
    }
    file->data = archive_file.data;
    file->size = archive_file.size;
    file->offset = 0;
    file->first_cluster = 0;
    file->directory_cluster = 0;
    file->entry_index = 0;
    file->open = true;
    file->block_backed = false;
    return true;
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

    if (file->block_backed)
    {
        const FAT16::File disk_file = DiskFile(*file);
        uint64_t count;
        if (!FAT16::Read(&disk_file, file->offset, buffer, length, &count))
        {
            return false;
        }
        file->offset += count;
        *bytes_read = count;
        return true;
    }

    const uint64_t available = file->size - file->offset;
    const uint64_t count = length < available ? length : available;
    auto* destination = static_cast<uint8_t*>(buffer);
    for (uint64_t index = 0; index < count; ++index)
    {
        destination[index] = file->data[file->offset + index];
    }
    file->offset += count;
    *bytes_read = count;
    return true;
}

bool Seek(FileHandle* file, uint64_t offset)
{
    if (file == nullptr || !file->open || offset > file->size)
    {
        return false;
    }
    file->offset = offset;
    return true;
}

bool Stat(const char* path, FileInfo* info)
{
    if (path == nullptr || info == nullptr)
    {
        return false;
    }

    if (const char* volume_path = DiskEntryPath(path))
    {
        FAT16::FileInfo disk_file_info;
        if (!FAT16::Stat(volume_path, &disk_file_info))
        {
            return false;
        }
        info->size = disk_file_info.size;
        return true;
    }

    Initramfs::File archive_file;
    if (!Initramfs::Open(ArchivePath(path), &archive_file))
    {
        return false;
    }
    info->size = archive_file.size;
    return true;
}

void Close(FileHandle* file)
{
    if (file == nullptr)
    {
        return;
    }
    file->data = nullptr;
    file->size = 0;
    file->offset = 0;
    file->first_cluster = 0;
    file->directory_cluster = 0;
    file->entry_index = 0;
    file->open = false;
    file->block_backed = false;
}

bool Create(const char* path, FileHandle* file)
{
    FAT16::File disk_file;
    const char* volume_path = path == nullptr ? nullptr : DiskEntryPath(path);
    if (volume_path == nullptr || file == nullptr || !FAT16::Create(volume_path, &disk_file))
    {
        return false;
    }
    OpenDiskHandle(disk_file, file);
    return true;
}

bool Write(FileHandle* file, const void* buffer, uint64_t length, uint64_t* bytes_written)
{
    if (bytes_written == nullptr)
    {
        return false;
    }
    *bytes_written = 0;
    if (file == nullptr || !file->open || !file->block_backed)
    {
        return false;
    }
    FAT16::File disk_file = DiskFile(*file);
    const bool written = FAT16::Write(&disk_file, file->offset, buffer, length, bytes_written);
    UpdateHandle(disk_file, file);
    file->offset += *bytes_written;
    return written;
}

bool Truncate(FileHandle* file, uint64_t size)
{
    if (file == nullptr || !file->open || !file->block_backed)
    {
        return false;
    }
    FAT16::File disk_file = DiskFile(*file);
    const bool truncated = FAT16::Truncate(&disk_file, size);
    UpdateHandle(disk_file, file);
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
    const char* volume_path = VolumePath(path);
    if (volume_path != nullptr && *volume_path == '\0')
    {
        // The root lists the initramfs mount point, then the disk's own root entries.
        if (index == 0)
        {
            constexpr char MountName[] = "system";
            for (uint32_t offset = 0; offset < sizeof(MountName); ++offset)
            {
                entry->name[offset] = MountName[offset];
            }
            entry->directory = true;
            entry->size = 0;
            return true;
        }
        return FAT16::ReadDirectory("", index - 1, entry);
    }
    if (volume_path != nullptr)
    {
        return FAT16::ReadDirectory(volume_path, index, entry);
    }
    uint64_t size;
    if (*ArchivePath(path) != '\0' || !Initramfs::ReadDirectory(index, entry->name, &size))
    {
        return false;
    }
    entry->directory = false;
    entry->size = static_cast<uint32_t>(size);
    return true;
}

bool IsDirectory(const char* path)
{
    if (path == nullptr)
    {
        return false;
    }
    const char* volume_path = VolumePath(path);
    if (volume_path == nullptr)
    {
        return path[0] == '/' && *ArchivePath(path) == '\0';
    }
    // The root exists even without a disk, since it holds /system.
    return *volume_path == '\0' || FAT16::IsDirectory(volume_path);
}

bool MakeDirectory(const char* path)
{
    const char* volume_path = path == nullptr ? nullptr : DiskEntryPath(path);
    return volume_path != nullptr && FAT16::MakeDirectory(volume_path);
}

bool RemoveDirectory(const char* path)
{
    const char* volume_path = path == nullptr ? nullptr : DiskEntryPath(path);
    return volume_path != nullptr && FAT16::RemoveDirectory(volume_path);
}

bool Remove(const char* path)
{
    const char* volume_path = path == nullptr ? nullptr : DiskEntryPath(path);
    return volume_path != nullptr && FAT16::Remove(volume_path);
}

} // namespace FeatherOS::VFS