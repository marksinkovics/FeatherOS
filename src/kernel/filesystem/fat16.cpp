#include "fat16.h"

#include "kernel/storage/block_device.h"

#include <stddef.h>

namespace FeatherOS::FAT16
{
namespace
{

constexpr uint32_t BytesPerSector = 512;
constexpr uint32_t PartitionTableOffset = 446;
constexpr uint32_t PartitionEntrySize = 16;
constexpr uint8_t Fat16PartitionType = 0x06;
constexpr uint8_t Fat16LbaPartitionType = 0x0E;
constexpr uint16_t Fat16EndOfChain = 0xFFF8;
constexpr uint32_t RootEntrySize = 32;

uint64_t partition_start = 0;
uint32_t partition_sectors = 0;
uint32_t reserved_sectors = 0;
uint32_t fat_count = 0;
uint32_t fat_size_sectors = 0;
uint32_t root_entry_count = 0;
uint32_t root_directory_sectors = 0;
uint32_t data_start_sector = 0;
uint32_t sectors_per_cluster = 0;
uint32_t cluster_count = 0;
uint32_t cluster_size = 0;
bool mounted = false;

uint16_t ReadLittleEndian16(const uint8_t* bytes)
{
    return static_cast<uint16_t>(bytes[0]) |
           static_cast<uint16_t>(static_cast<uint16_t>(bytes[1]) << 8);
}

uint32_t ReadLittleEndian32(const uint8_t* bytes)
{
    return static_cast<uint32_t>(bytes[0]) | (static_cast<uint32_t>(bytes[1]) << 8) |
           (static_cast<uint32_t>(bytes[2]) << 16) | (static_cast<uint32_t>(bytes[3]) << 24);
}

bool IsFat16Partition(uint8_t partition_type)
{
    return partition_type == Fat16PartitionType || partition_type == Fat16LbaPartitionType;
}

bool MountPartition(uint32_t start_sector, uint32_t sector_count, uint8_t* boot_sector)
{
    if (sector_count == 0 || !BlockDevice::ReadSector(start_sector, boot_sector) ||
        ReadLittleEndian16(boot_sector + 510) != 0xAA55 ||
        ReadLittleEndian16(boot_sector + 11) != BytesPerSector)
    {
        return false;
    }

    const uint32_t new_sectors_per_cluster = boot_sector[13];
    const uint32_t new_reserved_sectors = ReadLittleEndian16(boot_sector + 14);
    const uint32_t new_fat_count = boot_sector[16];
    const uint32_t new_root_entry_count = ReadLittleEndian16(boot_sector + 17);
    const uint32_t total_sectors_16 = ReadLittleEndian16(boot_sector + 19);
    const uint32_t new_fat_size = ReadLittleEndian16(boot_sector + 22);
    const uint32_t total_sectors =
        total_sectors_16 != 0 ? total_sectors_16 : ReadLittleEndian32(boot_sector + 32);
    if (new_sectors_per_cluster == 0 ||
        (new_sectors_per_cluster & (new_sectors_per_cluster - 1)) != 0 ||
        new_reserved_sectors == 0 || new_fat_count == 0 || new_root_entry_count == 0 ||
        new_fat_size == 0 || total_sectors == 0 || total_sectors > sector_count)
    {
        return false;
    }

    const uint32_t new_root_directory_sectors =
        (new_root_entry_count * RootEntrySize + BytesPerSector - 1) / BytesPerSector;
    const uint64_t metadata_sectors = static_cast<uint64_t>(new_reserved_sectors) +
                                      static_cast<uint64_t>(new_fat_count) * new_fat_size +
                                      new_root_directory_sectors;
    if (metadata_sectors >= total_sectors || metadata_sectors > UINT32_MAX)
    {
        return false;
    }

    const uint32_t new_cluster_count =
        static_cast<uint32_t>((total_sectors - metadata_sectors) / new_sectors_per_cluster);
    if (new_cluster_count < 4085 || new_cluster_count >= 65525)
    {
        return false;
    }

    partition_start = start_sector;
    partition_sectors = total_sectors;
    reserved_sectors = new_reserved_sectors;
    fat_count = new_fat_count;
    fat_size_sectors = new_fat_size;
    root_entry_count = new_root_entry_count;
    root_directory_sectors = new_root_directory_sectors;
    data_start_sector = static_cast<uint32_t>(metadata_sectors);
    sectors_per_cluster = new_sectors_per_cluster;
    cluster_count = new_cluster_count;
    cluster_size = BytesPerSector * sectors_per_cluster;
    mounted = true;
    return true;
}

bool MakeShortName(const char* path, uint8_t name[11])
{
    for (uint32_t index = 0; index < 11; ++index)
    {
        name[index] = ' ';
    }
    if (path == nullptr || path[0] == '\0')
    {
        return false;
    }

    uint32_t base_length = 0;
    uint32_t extension_length = 0;
    bool extension = false;
    for (uint32_t index = 0; path[index] != '\0'; ++index)
    {
        char character = path[index];
        if (character == '/')
        {
            return false;
        }
        if (character == '.' && !extension)
        {
            extension = true;
            continue;
        }
        if (character == '.' || character == ' ' || character < 0x21 || character > 0x7e)
        {
            return false;
        }
        if (character >= 'a' && character <= 'z')
        {
            character = static_cast<char>(character - 'a' + 'A');
        }
        if (extension)
        {
            if (extension_length == 3)
            {
                return false;
            }
            name[8 + extension_length++] = static_cast<uint8_t>(character);
        }
        else
        {
            if (base_length == 8)
            {
                return false;
            }
            name[base_length++] = static_cast<uint8_t>(character);
        }
    }
    return base_length > 0;
}

bool FindRootFile(const char* path, File* file)
{
    uint8_t target_name[11];
    if (!MakeShortName(path, target_name))
    {
        return false;
    }

    uint8_t sector[BytesPerSector];
    for (uint32_t sector_index = 0; sector_index < root_directory_sectors; ++sector_index)
    {
        const uint64_t root_sector = partition_start + reserved_sectors +
                                     static_cast<uint64_t>(fat_count) * fat_size_sectors +
                                     sector_index;
        if (!BlockDevice::ReadSector(root_sector, sector))
        {
            return false;
        }
        for (uint32_t entry_index = 0; entry_index < BytesPerSector / RootEntrySize; ++entry_index)
        {
            const uint8_t* entry = sector + entry_index * RootEntrySize;
            if (entry[0] == 0)
            {
                return false;
            }
            if (entry[0] == 0xE5 || entry[11] == 0x0F || (entry[11] & 0x18) != 0)
            {
                continue;
            }

            bool matches = true;
            for (uint32_t index = 0; index < sizeof(target_name); ++index)
            {
                if (entry[index] != target_name[index])
                {
                    matches = false;
                    break;
                }
            }
            if (matches)
            {
                file->first_cluster = ReadLittleEndian16(entry + 26);
                file->size = ReadLittleEndian32(entry + 28);
                return file->size == 0 ||
                       (file->first_cluster >= 2 && file->first_cluster < cluster_count + 2);
            }
        }
    }
    return false;
}

bool NextCluster(uint32_t cluster, uint32_t* next_cluster)
{
    const uint64_t fat_offset = static_cast<uint64_t>(cluster) * 2;
    const uint64_t fat_sector = partition_start + reserved_sectors + fat_offset / BytesPerSector;
    uint8_t sector[BytesPerSector];
    if (fat_offset / BytesPerSector >= fat_size_sectors ||
        !BlockDevice::ReadSector(fat_sector, sector))
    {
        return false;
    }
    const uint16_t next = ReadLittleEndian16(sector + fat_offset % BytesPerSector);
    if (next >= Fat16EndOfChain)
    {
        return false;
    }
    if (next < 2 || next >= cluster_count + 2)
    {
        return false;
    }
    *next_cluster = next;
    return true;
}

bool ReadFile(const File& file, uint64_t offset, void* buffer, uint64_t length,
              uint64_t* bytes_read)
{
    if (bytes_read == nullptr)
    {
        return false;
    }
    *bytes_read = 0;
    if ((length != 0 && buffer == nullptr) || offset > file.size)
    {
        return false;
    }

    uint64_t remaining = file.size - offset;
    if (remaining > length)
    {
        remaining = length;
    }
    if (remaining == 0)
    {
        return true;
    }

    uint32_t cluster = file.first_cluster;
    uint64_t cluster_offset = offset;
    while (cluster_offset >= cluster_size)
    {
        uint32_t next_cluster;
        if (!NextCluster(cluster, &next_cluster))
        {
            return false;
        }
        cluster = next_cluster;
        cluster_offset -= cluster_size;
    }

    auto* destination = static_cast<uint8_t*>(buffer);
    uint8_t sector[BytesPerSector];
    uint64_t copied = 0;
    while (remaining > 0)
    {
        const uint32_t sector_in_cluster = static_cast<uint32_t>(cluster_offset / BytesPerSector);
        const uint32_t offset_in_sector = static_cast<uint32_t>(cluster_offset % BytesPerSector);
        const uint64_t data_sector = partition_start + data_start_sector +
                                     static_cast<uint64_t>(cluster - 2) * sectors_per_cluster +
                                     sector_in_cluster;
        if (!BlockDevice::ReadSector(data_sector, sector))
        {
            return false;
        }
        uint64_t chunk = BytesPerSector - offset_in_sector;
        if (chunk > remaining)
        {
            chunk = remaining;
        }
        for (uint64_t index = 0; index < chunk; ++index)
        {
            destination[copied + index] = sector[offset_in_sector + index];
        }
        copied += chunk;
        remaining -= chunk;
        cluster_offset += chunk;
        if (remaining > 0 && cluster_offset == cluster_size)
        {
            uint32_t next_cluster;
            if (!NextCluster(cluster, &next_cluster))
            {
                return false;
            }
            cluster = next_cluster;
            cluster_offset = 0;
        }
    }
    *bytes_read = copied;
    return true;
}

} // namespace

bool Mount()
{
    mounted = false;
    uint8_t mbr[BytesPerSector];
    if (!BlockDevice::ReadSector(0, mbr) || mbr[510] != 0x55 || mbr[511] != 0xAA)
    {
        return false;
    }

    uint8_t boot_sector[BytesPerSector];
    for (uint32_t index = 0; index < 4; ++index)
    {
        const uint8_t* entry = mbr + PartitionTableOffset + index * PartitionEntrySize;
        if (!IsFat16Partition(entry[4]))
        {
            continue;
        }
        const uint32_t start_sector = ReadLittleEndian32(entry + 8);
        const uint32_t sector_count = ReadLittleEndian32(entry + 12);
        if (start_sector == 0 || sector_count == 0 || start_sector > BlockDevice::SectorCount() ||
            sector_count > BlockDevice::SectorCount() - start_sector)
        {
            continue;
        }
        if (MountPartition(start_sector, sector_count, boot_sector))
        {
            return true;
        }
    }
    return false;
}

bool Open(const char* path, File* file)
{
    return mounted && file != nullptr && FindRootFile(path, file);
}

bool Stat(const char* path, FileInfo* info)
{
    if (!mounted || info == nullptr)
    {
        return false;
    }
    File file;
    if (!FindRootFile(path, &file))
    {
        return false;
    }
    info->size = file.size;
    return true;
}

bool Read(const File* file, uint64_t offset, void* buffer, uint64_t length, uint64_t* bytes_read)
{
    return mounted && file != nullptr && ReadFile(*file, offset, buffer, length, bytes_read);
}

} // namespace FeatherOS::FAT16