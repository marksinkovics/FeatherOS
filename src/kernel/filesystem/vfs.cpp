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
        file->data = nullptr;
        file->size = disk_file.size;
        file->offset = 0;
        file->first_cluster = disk_file.first_cluster;
        file->open = true;
        file->block_backed = true;
        return true;
    }

    Initramfs::File archive_file;
    if (!Initramfs::Open(path, &archive_file))
    {
        return false;
    }
    file->data = archive_file.data;
    file->size = archive_file.size;
    file->offset = 0;
    file->first_cluster = 0;
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
        FAT16::File disk_file = {file->first_cluster, static_cast<uint32_t>(file->size)};
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
    if (!Initramfs::Open(path, &archive_file))
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
    file->open = false;
    file->block_backed = false;
}

} // namespace FeatherOS::VFS