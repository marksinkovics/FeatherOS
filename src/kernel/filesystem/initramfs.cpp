#include "initramfs.h"

#include <stddef.h>

namespace FeatherOS::Initramfs
{
namespace
{

constexpr uint64_t TarBlockSize = 512;
constexpr size_t FullPathCapacity = 155 + 1 + 100 + 1;

struct __attribute__((packed)) TarHeader
{
    char name[100];
    char mode[8];
    char owner_id[8];
    char group_id[8];
    char size[12];
    char modified_time[12];
    char checksum[8];
    char type;
    char link_name[100];
    char magic[6];
    char version[2];
    char owner_name[32];
    char group_name[32];
    char device_major[8];
    char device_minor[8];
    char prefix[155];
    char padding[12];
};

static_assert(sizeof(TarHeader) == TarBlockSize, "USTAR headers are 512 bytes");

const uint8_t* archive_data = nullptr;
uint64_t archive_size = 0;

bool ParseOctal(const char* field, size_t field_size, uint64_t* value)
{
    size_t index = 0;
    while (index < field_size && (field[index] == '\0' || field[index] == ' '))
    {
        ++index;
    }
    if (index == field_size)
    {
        return false;
    }

    uint64_t result = 0;
    for (; index < field_size && field[index] != '\0' && field[index] != ' '; ++index)
    {
        if (field[index] < '0' || field[index] > '7' || result > (UINT64_MAX >> 3))
        {
            return false;
        }
        result = (result << 3) | static_cast<uint64_t>(field[index] - '0');
    }
    *value = result;
    return true;
}

bool IsZeroBlock(const uint8_t* block)
{
    for (uint64_t index = 0; index < TarBlockSize; ++index)
    {
        if (block[index] != 0)
        {
            return false;
        }
    }
    return true;
}

bool HasValidChecksum(const TarHeader& header)
{
    uint64_t expected_checksum;
    if (!ParseOctal(header.checksum, sizeof(header.checksum), &expected_checksum))
    {
        return false;
    }

    const auto* bytes = reinterpret_cast<const uint8_t*>(&header);
    uint64_t checksum = 0;
    for (uint64_t index = 0; index < TarBlockSize; ++index)
    {
        if (index >= offsetof(TarHeader, checksum) &&
            index < offsetof(TarHeader, checksum) + sizeof(header.checksum))
        {
            checksum += ' ';
        }
        else
        {
            checksum += bytes[index];
        }
    }
    return checksum == expected_checksum;
}

// The prefix field holds the leading directories of a path too long for the name field.
bool FullPath(const TarHeader& header, char (&path)[FullPathCapacity])
{
    size_t length = 0;
    for (size_t index = 0; index < sizeof(header.prefix) && header.prefix[index] != '\0'; ++index)
    {
        path[length++] = header.prefix[index];
    }
    if (length > 0)
    {
        path[length++] = '/';
    }
    for (size_t index = 0; index < sizeof(header.name) && header.name[index] != '\0'; ++index)
    {
        path[length++] = header.name[index];
    }
    path[length] = '\0';
    return length > 0;
}

// The part of path inside directory, or nullptr when path is not in it.
const char* InDirectory(const char* path, const char* directory)
{
    for (; *directory != '\0'; ++directory, ++path)
    {
        if (*path != *directory)
        {
            return nullptr;
        }
    }
    return *path == '/' ? path + 1 : nullptr;
}

bool Equal(const char* left, const char* right)
{
    for (; *left != '\0' && *left == *right; ++left, ++right)
    {
    }
    return *left == *right;
}

// Calls visit(header, data, size) for each regular file until it returns true; false if none
// does or the archive is malformed.
template <typename Visit> bool ForEachFile(Visit visit)
{
    if (archive_data == nullptr)
    {
        return false;
    }
    uint64_t offset = 0;
    while (offset <= archive_size && archive_size - offset >= TarBlockSize)
    {
        const uint8_t* block = archive_data + offset;
        if (IsZeroBlock(block))
        {
            return false;
        }

        const auto* header = reinterpret_cast<const TarHeader*>(block);
        if (header->magic[0] != 'u' || header->magic[1] != 's' || header->magic[2] != 't' ||
            header->magic[3] != 'a' || header->magic[4] != 'r' || !HasValidChecksum(*header))
        {
            return false;
        }

        uint64_t file_size;
        if (!ParseOctal(header->size, sizeof(header->size), &file_size) ||
            file_size > UINT64_MAX - (TarBlockSize - 1))
        {
            return false;
        }
        const uint64_t padded_size = (file_size + TarBlockSize - 1) & ~(TarBlockSize - 1);
        if (padded_size > archive_size - offset - TarBlockSize)
        {
            return false;
        }
        if ((header->type == '\0' || header->type == '0') &&
            visit(*header, block + TarBlockSize, file_size))
        {
            return true;
        }
        offset += TarBlockSize + padded_size;
    }
    return false;
}

} // namespace

bool Mount(const void* archive, uint64_t size)
{
    if (archive == nullptr || size < TarBlockSize * 2)
    {
        return false;
    }
    archive_data = static_cast<const uint8_t*>(archive);
    archive_size = size;
    return true;
}

bool Open(const char* directory, const char* name, File* file)
{
    if (directory == nullptr || name == nullptr || name[0] == '\0' || file == nullptr)
    {
        return false;
    }
    return ForEachFile(
        [&](const TarHeader& header, const uint8_t* data, uint64_t size)
        {
            char path[FullPathCapacity];
            const char* inner = FullPath(header, path) ? InDirectory(path, directory) : nullptr;
            if (inner == nullptr || !Equal(inner, name))
            {
                return false;
            }
            file->data = data;
            file->size = size;
            return true;
        });
}

bool ReadDirectory(const char* directory, uint32_t index, char (&name)[64], uint64_t* size)
{
    if (directory == nullptr || size == nullptr)
    {
        return false;
    }
    uint32_t listed = 0;
    return ForEachFile(
        [&](const TarHeader& header, const uint8_t*, uint64_t file_size)
        {
            char path[FullPathCapacity];
            const char* inner = FullPath(header, path) ? InDirectory(path, directory) : nullptr;
            if (inner == nullptr)
            {
                return false;
            }
            size_t length = 0;
            while (inner[length] != '\0' && inner[length] != '/')
            {
                ++length;
            }
            if (length == 0 || inner[length] == '/' || length >= sizeof(name) || listed++ != index)
            {
                return false;
            }
            for (size_t character = 0; character < length; ++character)
            {
                name[character] = inner[character];
            }
            name[length] = '\0';
            *size = file_size;
            return true;
        });
}

} // namespace FeatherOS::Initramfs
