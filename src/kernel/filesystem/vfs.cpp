#include "vfs.h"

#include "fat16.h"
#include "initramfs.h"

namespace FeatherOS::VFS
{
namespace
{

constexpr char DiskMountPath[] = "/disk/";

bool IsDiskPath(const char* path)
{
    for (uint32_t index = 0; index < sizeof(DiskMountPath) - 1; ++index)
    {
        if (path[index] != DiskMountPath[index])
        {
            return false;
        }
    }
    return path[sizeof(DiskMountPath) - 1] != '\0';
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

// The initramfs is the read-only root: "/hello.txt" names its "hello.txt". Kernel code may also
// use the bare name.
const char* ArchivePath(const char* path)
{
    return path[0] == '/' ? path + 1 : path;
}

// The part of a /disk path inside the volume: "" for "/disk" itself, or nullptr otherwise.
const char* VolumePath(const char* path)
{
    for (uint32_t index = 0; index < sizeof(DiskMountPath) - 2; ++index)
    {
        if (path[index] != DiskMountPath[index])
        {
            return nullptr;
        }
    }
    const char* rest = path + sizeof(DiskMountPath) - 2;
    return rest[0] == '\0' || rest[0] == '/' ? rest : nullptr;
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

    if (IsDiskPath(path))
    {
        FAT16::File disk_file;
        if (!FAT16::Open(path + sizeof(DiskMountPath) - 1, &disk_file))
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

    if (IsDiskPath(path))
    {
        FAT16::FileInfo disk_file_info;
        if (!FAT16::Stat(path + sizeof(DiskMountPath) - 1, &disk_file_info))
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
    if (path == nullptr || file == nullptr || !IsDiskPath(path) ||
        !FAT16::Create(path + sizeof(DiskMountPath) - 1, &disk_file))
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
    if (path != nullptr && entry != nullptr && path[0] == '/' && path[1] == '\0')
    {
        // The root lists the disk's mount point, then the initramfs files.
        if (index == 0)
        {
            constexpr char MountName[] = "disk";
            for (uint32_t offset = 0; offset < sizeof(MountName); ++offset)
            {
                entry->name[offset] = MountName[offset];
            }
            entry->directory = true;
            entry->size = 0;
            return true;
        }
        uint64_t size;
        if (!Initramfs::ReadDirectory(index - 1, entry->name, &size))
        {
            return false;
        }
        entry->directory = false;
        entry->size = static_cast<uint32_t>(size);
        return true;
    }
    const char* volume_path = path == nullptr ? nullptr : VolumePath(path);
    return volume_path != nullptr && FAT16::ReadDirectory(volume_path, index, entry);
}

bool IsDirectory(const char* path)
{
    if (path != nullptr && path[0] == '/' && path[1] == '\0')
    {
        return true;
    }
    const char* volume_path = path == nullptr ? nullptr : VolumePath(path);
    return volume_path != nullptr && FAT16::IsDirectory(volume_path);
}

bool MakeDirectory(const char* path)
{
    return path != nullptr && IsDiskPath(path) &&
           FAT16::MakeDirectory(path + sizeof(DiskMountPath) - 1);
}

bool RemoveDirectory(const char* path)
{
    return path != nullptr && IsDiskPath(path) &&
           FAT16::RemoveDirectory(path + sizeof(DiskMountPath) - 1);
}

bool Remove(const char* path)
{
    return path != nullptr && IsDiskPath(path) && FAT16::Remove(path + sizeof(DiskMountPath) - 1);
}

} // namespace FeatherOS::VFS