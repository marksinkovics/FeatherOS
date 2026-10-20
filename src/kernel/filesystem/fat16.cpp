#include "fat16.h"

#include "kernel/filesystem/fat_names.h"
#include "kernel/storage/block_device.h"
#include "kernel/storage/partition.h"

#include <stddef.h>

namespace FeatherOS::FAT16
{
namespace
{

constexpr uint32_t BytesPerSector = 512;
constexpr uint16_t Fat16EndOfChain = 0xFFF8;
constexpr uint16_t Fat16EndOfChainMark = 0xFFFF;
constexpr uint16_t Fat16FreeCluster = 0;
constexpr uint32_t RootEntrySize = 32;
constexpr uint32_t EntriesPerSector = BytesPerSector / RootEntrySize;
constexpr uint32_t FatEntriesPerSector = BytesPerSector / 2;
constexpr uint8_t EndOfDirectory = 0x00;
constexpr uint8_t DeletedEntry = 0xE5;
constexpr uint8_t ArchiveAttribute = 0x20;
constexpr uint8_t DirectoryAttribute = 0x10;
constexpr uint8_t VolumeLabelAttribute = 0x08;
constexpr uint32_t NoEntry = UINT32_MAX;
// Directory cluster 0 names the root directory, which sits in its own fixed region.
constexpr uint32_t RootDirectory = 0;
// FAT limits a directory to 65536 entries; the bound also stops a looping cluster chain.
constexpr uint32_t MaximumDirectoryEntries = 65536;
constexpr uint32_t MaximumPathLength = 128;

enum class Lookup : uint8_t
{
    Found,
    Missing,
    Error,
};

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

void WriteLittleEndian16(uint8_t* bytes, uint16_t value)
{
    bytes[0] = static_cast<uint8_t>(value);
    bytes[1] = static_cast<uint8_t>(value >> 8);
}

void WriteLittleEndian32(uint8_t* bytes, uint32_t value)
{
    for (uint32_t index = 0; index < 4; ++index)
    {
        bytes[index] = static_cast<uint8_t>(value >> (8 * index));
    }
}

bool MountPartition(uint64_t start_sector, uint64_t sector_count, uint8_t* boot_sector)
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

bool IsDataCluster(uint32_t cluster)
{
    return cluster >= 2 && cluster < cluster_count + 2;
}

uint64_t RootSector(uint32_t entry_index)
{
    return partition_start + reserved_sectors +
           static_cast<uint64_t>(fat_count) * fat_size_sectors + entry_index / EntriesPerSector;
}

uint64_t ClusterSector(uint32_t cluster)
{
    return partition_start + data_start_sector +
           static_cast<uint64_t>(cluster - 2) * sectors_per_cluster;
}

bool NextCluster(uint32_t cluster, uint32_t* next_cluster);

// A directory entry's position: its directory's first cluster and its index there.
struct Slot
{
    uint32_t directory;
    uint32_t index;
};

uint32_t EntriesPerCluster()
{
    return EntriesPerSector * sectors_per_cluster;
}

// False past the end of the directory, including where its cluster chain stops.
bool SlotSector(Slot slot, uint64_t* sector)
{
    if (slot.directory == RootDirectory)
    {
        if (slot.index >= root_entry_count)
        {
            return false;
        }
        *sector = RootSector(slot.index);
        return true;
    }
    if (slot.index >= MaximumDirectoryEntries)
    {
        return false;
    }
    uint32_t cluster = slot.directory;
    for (uint32_t step = 0; step < slot.index / EntriesPerCluster(); ++step)
    {
        if (!NextCluster(cluster, &cluster))
        {
            return false;
        }
    }
    *sector = ClusterSector(cluster) + (slot.index % EntriesPerCluster()) / EntriesPerSector;
    return true;
}

// Calls visit(entry, slot) on each slot in order until it returns true (Found) or the directory
// ends (Missing). The end-of-directory slot is visited too, so callers can claim it.
template <typename Visit> Lookup ScanDirectory(uint32_t directory, Visit visit)
{
    uint8_t sector[BytesPerSector];
    uint64_t loaded_sector = UINT64_MAX;
    for (uint32_t index = 0;; ++index)
    {
        uint64_t sector_number;
        if (!SlotSector({directory, index}, &sector_number))
        {
            return Lookup::Missing;
        }
        if (sector_number != loaded_sector)
        {
            if (!BlockDevice::ReadSector(sector_number, sector))
            {
                return Lookup::Error;
            }
            loaded_sector = sector_number;
        }
        const uint8_t* entry = sector + (index % EntriesPerSector) * RootEntrySize;
        if (visit(entry, Slot{directory, index}))
        {
            return Lookup::Found;
        }
        if (entry[0] == EndOfDirectory)
        {
            return Lookup::Missing;
        }
    }
}

bool IsDotEntry(const uint8_t* entry)
{
    return entry[0] == '.' && (entry[1] == ' ' || (entry[1] == '.' && entry[2] == ' '));
}

using Name = char[FatNames::MaximumNameLength + 1];

// Calls visit(name, entry, slot, first_slot) for each file or directory entry, "." and ".."
// included. name is the long name when a valid one precedes the entry, otherwise the short one,
// and first_slot is where the entry's long-name entries start.
template <typename Visit> Lookup ScanNames(uint32_t directory, Visit visit)
{
    FatNames::LongNameReader reader;
    reader.Reset();
    return ScanDirectory(directory,
                         [&](const uint8_t* entry, Slot slot)
                         {
                             if (entry[0] == EndOfDirectory || entry[0] == DeletedEntry ||
                                 (entry[11] != FatNames::LongNameAttribute &&
                                  (entry[11] & VolumeLabelAttribute) != 0))
                             {
                                 reader.Reset();
                                 return false;
                             }
                             if (entry[11] == FatNames::LongNameAttribute)
                             {
                                 reader.Feed(entry, slot.index);
                                 return false;
                             }
                             Name name;
                             uint32_t first_slot = slot.index;
                             if (!reader.Finish(entry, name, &first_slot))
                             {
                                 FatNames::ShortNameText(entry, name);
                             }
                             reader.Reset();
                             return visit(name, entry, slot, first_slot);
                         });
}

struct Found
{
    File file;
    uint8_t attributes;
    // The first of the entry's long-name entries, or the entry itself without a long name.
    uint32_t first_slot;
};

// A name matches an entry's long name or its short name, ignoring case.
Lookup FindEntry(uint32_t directory, const char* target, Found* found)
{
    bool valid = true;
    const Lookup result = ScanNames(
        directory,
        [&](const char* name, const uint8_t* entry, Slot slot, uint32_t first_slot)
        {
            Name short_name;
            FatNames::ShortNameText(entry, short_name);
            if (!FatNames::SameName(name, target) && !FatNames::SameName(short_name, target))
            {
                return false;
            }
            File& file = found->file;
            file.first_cluster = ReadLittleEndian16(entry + 26);
            file.size = ReadLittleEndian32(entry + 28);
            file.directory_cluster = slot.directory;
            file.entry_index = slot.index;
            found->attributes = entry[11];
            found->first_slot = first_slot;
            // A ".." entry that points at the root records cluster 0.
            valid = (entry[11] & DirectoryAttribute) != 0
                        ? file.first_cluster == RootDirectory || IsDataCluster(file.first_cluster)
                        : (file.first_cluster == 0 ? file.size == 0
                                                   : IsDataCluster(file.first_cluster));
            return true;
        });
    return result == Lookup::Found && !valid ? Lookup::Error : result;
}

// "." stays, and ".." in the root stays in the root.
bool EnterDirectory(uint32_t* directory, const char* name)
{
    if (FatNames::SameName(name, ".") || (FatNames::SameName(name, "..") && *directory == 0))
    {
        return true;
    }
    Found found;
    if (FindEntry(*directory, name, &found) != Lookup::Found ||
        (found.attributes & DirectoryAttribute) == 0)
    {
        return false;
    }
    *directory = found.file.first_cluster;
    return true;
}

// Walks every component of path except the last, which *leaf then points at inside buffer. Empty
// components (from "//" or a trailing '/') are skipped; *leaf is empty when path names the root.
bool WalkPath(const char* path, char (&buffer)[MaximumPathLength], uint32_t* directory,
              const char** leaf)
{
    *directory = RootDirectory;
    uint32_t length = 0;
    for (; path[length] != '\0'; ++length)
    {
        if (length == MaximumPathLength - 1)
        {
            return false;
        }
        buffer[length] = path[length];
    }
    while (length > 0 && buffer[length - 1] == '/')
    {
        --length;
    }
    buffer[length] = '\0';
    char* component = buffer;
    for (;;)
    {
        while (*component == '/')
        {
            ++component;
        }
        char* end = component;
        while (*end != '\0' && *end != '/')
        {
            ++end;
        }
        if (*end == '\0')
        {
            *leaf = component;
            return true;
        }
        *end = '\0';
        if (!EnterDirectory(directory, component))
        {
            return false;
        }
        component = end + 1;
    }
}

bool ResolveDirectory(const char* path, uint32_t* directory)
{
    char buffer[MaximumPathLength];
    const char* leaf;
    return WalkPath(path, buffer, directory, &leaf) &&
           (*leaf == '\0' || EnterDirectory(directory, leaf));
}

// *directory is where the entry is, or where it would be created; *leaf is its name.
Lookup FindPath(const char* path, char (&buffer)[MaximumPathLength], uint32_t* directory,
                const char** leaf, Found* found)
{
    if (!WalkPath(path, buffer, directory, leaf))
    {
        return Lookup::Error;
    }
    return **leaf == '\0' ? Lookup::Error : FindEntry(*directory, *leaf, found);
}

bool ReadSlot(Slot slot, uint8_t (&sector)[BytesPerSector], uint64_t* sector_number,
              uint8_t** entry)
{
    if (!SlotSector(slot, sector_number) || !BlockDevice::ReadSector(*sector_number, sector))
    {
        return false;
    }
    *entry = sector + (slot.index % EntriesPerSector) * RootEntrySize;
    return true;
}

bool UpdateEntry(const File& file)
{
    uint8_t sector[BytesPerSector];
    uint64_t sector_number;
    uint8_t* entry;
    if (!ReadSlot({file.directory_cluster, file.entry_index}, sector, &sector_number, &entry))
    {
        return false;
    }
    WriteLittleEndian16(entry + 26, static_cast<uint16_t>(file.first_cluster));
    WriteLittleEndian32(entry + 28, file.size);
    return BlockDevice::WriteSector(sector_number, sector);
}

bool ReadFatEntry(uint32_t cluster, uint16_t* value)
{
    const uint64_t fat_offset = static_cast<uint64_t>(cluster) * 2;
    uint8_t sector[BytesPerSector];
    if (fat_offset / BytesPerSector >= fat_size_sectors ||
        !BlockDevice::ReadSector(partition_start + reserved_sectors + fat_offset / BytesPerSector,
                                 sector))
    {
        return false;
    }
    *value = ReadLittleEndian16(sector + fat_offset % BytesPerSector);
    return true;
}

// Every FAT copy gets the same update, so disk checkers see them agree.
bool WriteFatEntry(uint32_t cluster, uint16_t value)
{
    const uint64_t fat_offset = static_cast<uint64_t>(cluster) * 2;
    if (fat_offset / BytesPerSector >= fat_size_sectors)
    {
        return false;
    }
    uint8_t sector[BytesPerSector];
    for (uint32_t copy = 0; copy < fat_count; ++copy)
    {
        const uint64_t sector_number = partition_start + reserved_sectors +
                                       static_cast<uint64_t>(copy) * fat_size_sectors +
                                       fat_offset / BytesPerSector;
        if (!BlockDevice::ReadSector(sector_number, sector))
        {
            return false;
        }
        WriteLittleEndian16(sector + fat_offset % BytesPerSector, value);
        if (!BlockDevice::WriteSector(sector_number, sector))
        {
            return false;
        }
    }
    return true;
}

bool NextCluster(uint32_t cluster, uint32_t* next_cluster)
{
    uint16_t next;
    if (!ReadFatEntry(cluster, &next) || next >= Fat16EndOfChain || !IsDataCluster(next))
    {
        return false;
    }
    *next_cluster = next;
    return true;
}

// New clusters are zeroed so a file never exposes a deleted file's old contents.
bool AllocateCluster(uint32_t* cluster)
{
    uint8_t sector[BytesPerSector];
    for (uint32_t fat_sector = 0; fat_sector < fat_size_sectors; ++fat_sector)
    {
        if (!BlockDevice::ReadSector(partition_start + reserved_sectors + fat_sector, sector))
        {
            return false;
        }
        for (uint32_t slot = 0; slot < FatEntriesPerSector; ++slot)
        {
            const uint32_t candidate = fat_sector * FatEntriesPerSector + slot;
            if (candidate < 2)
            {
                continue;
            }
            if (!IsDataCluster(candidate))
            {
                return false;
            }
            if (ReadLittleEndian16(sector + slot * 2) != Fat16FreeCluster)
            {
                continue;
            }
            uint8_t zeros[BytesPerSector];
            for (uint8_t& byte : zeros)
            {
                byte = 0;
            }
            for (uint32_t index = 0; index < sectors_per_cluster; ++index)
            {
                if (!BlockDevice::WriteSector(ClusterSector(candidate) + index, zeros))
                {
                    return false;
                }
            }
            if (!WriteFatEntry(candidate, Fat16EndOfChainMark))
            {
                return false;
            }
            *cluster = candidate;
            return true;
        }
    }
    return false;
}

// Follows the chain to its next cluster, adding one at the end if the chain stops here.
bool NextOrNewCluster(uint32_t* cluster)
{
    uint16_t next;
    if (!ReadFatEntry(*cluster, &next))
    {
        return false;
    }
    if (next >= Fat16EndOfChain)
    {
        uint32_t fresh;
        if (!AllocateCluster(&fresh) || !WriteFatEntry(*cluster, static_cast<uint16_t>(fresh)))
        {
            return false;
        }
        *cluster = fresh;
        return true;
    }
    if (!IsDataCluster(next))
    {
        return false;
    }
    *cluster = next;
    return true;
}

// A chain longer than the volume has clusters must contain a loop.
bool FreeChain(uint32_t first_cluster)
{
    uint32_t cluster = first_cluster;
    for (uint32_t steps = 0; steps < cluster_count; ++steps)
    {
        uint16_t next;
        if (!IsDataCluster(cluster) || !ReadFatEntry(cluster, &next) ||
            !WriteFatEntry(cluster, Fat16FreeCluster))
        {
            return false;
        }
        if (next >= Fat16EndOfChain)
        {
            return true;
        }
        cluster = next;
    }
    return false;
}

bool WriteFile(File* file, uint64_t offset, const uint8_t* source, uint64_t length,
               uint64_t* bytes_written)
{
    *bytes_written = 0;
    if (offset > file->size || (length != 0 && source == nullptr) || length > UINT32_MAX - offset)
    {
        return false;
    }
    if (length == 0)
    {
        return true;
    }
    if (file->first_cluster == 0)
    {
        uint32_t first_cluster;
        if (!AllocateCluster(&first_cluster))
        {
            return false;
        }
        file->first_cluster = first_cluster;
        if (!UpdateEntry(*file))
        {
            return false;
        }
    }

    uint32_t cluster = file->first_cluster;
    uint64_t cluster_number = 0;
    uint64_t position = offset;
    bool complete = true;
    uint8_t sector[BytesPerSector];
    while (*bytes_written < length)
    {
        for (; cluster_number < position / cluster_size; ++cluster_number)
        {
            if (!NextOrNewCluster(&cluster))
            {
                complete = false;
                break;
            }
        }
        if (!complete)
        {
            break;
        }
        const uint64_t cluster_offset = position % cluster_size;
        const uint32_t offset_in_sector = static_cast<uint32_t>(cluster_offset % BytesPerSector);
        const uint64_t sector_number = ClusterSector(cluster) + cluster_offset / BytesPerSector;
        uint64_t chunk = BytesPerSector - offset_in_sector;
        if (chunk > length - *bytes_written)
        {
            chunk = length - *bytes_written;
        }
        // A partial sector keeps the bytes around the new data.
        if (chunk < BytesPerSector && !BlockDevice::ReadSector(sector_number, sector))
        {
            complete = false;
            break;
        }
        for (uint64_t index = 0; index < chunk; ++index)
        {
            sector[offset_in_sector + index] = source[*bytes_written + index];
        }
        if (!BlockDevice::WriteSector(sector_number, sector))
        {
            complete = false;
            break;
        }
        *bytes_written += chunk;
        position += chunk;
    }
    if (position > file->size)
    {
        file->size = static_cast<uint32_t>(position);
        complete = UpdateEntry(*file) && complete;
    }
    return complete;
}

bool TruncateFile(File* file, uint64_t size)
{
    if (size > file->size)
    {
        return false;
    }
    if (size == file->size)
    {
        return true;
    }
    const uint32_t old_first_cluster = file->first_cluster;
    const uint64_t kept_clusters = (size + cluster_size - 1) / cluster_size;
    if (kept_clusters == 0)
    {
        file->first_cluster = 0;
        file->size = 0;
        return UpdateEntry(*file) && (old_first_cluster == 0 || FreeChain(old_first_cluster));
    }

    uint32_t last_cluster = old_first_cluster;
    for (uint64_t index = 1; index < kept_clusters; ++index)
    {
        if (!NextCluster(last_cluster, &last_cluster))
        {
            return false;
        }
    }
    uint16_t next;
    if (!ReadFatEntry(last_cluster, &next))
    {
        return false;
    }
    file->size = static_cast<uint32_t>(size);
    if (!UpdateEntry(*file))
    {
        return false;
    }
    if (next >= Fat16EndOfChain)
    {
        return true;
    }
    return WriteFatEntry(last_cluster, Fat16EndOfChainMark) && FreeChain(next);
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

// Adds a zeroed cluster, whose entries are all free, to the end of a subdirectory.
bool GrowDirectory(uint32_t directory)
{
    if (directory == RootDirectory)
    {
        return false;
    }
    uint32_t cluster = directory;
    uint32_t clusters = 1;
    for (uint32_t next; NextCluster(cluster, &next); cluster = next)
    {
        if (++clusters >= MaximumDirectoryEntries / EntriesPerCluster())
        {
            return false;
        }
    }
    return NextOrNewCluster(&cluster);
}

// Finds count consecutive free slots, growing a subdirectory when it runs out.
bool FindFreeRun(uint32_t directory, uint32_t count, Slot* first)
{
    uint8_t sector[BytesPerSector];
    uint64_t loaded_sector = UINT64_MAX;
    uint32_t run_start = 0;
    uint32_t run_length = 0;
    for (uint32_t index = 0; index < MaximumDirectoryEntries;)
    {
        uint64_t sector_number;
        if (!SlotSector({directory, index}, &sector_number))
        {
            if (!GrowDirectory(directory))
            {
                return false;
            }
            continue;
        }
        if (sector_number != loaded_sector)
        {
            if (!BlockDevice::ReadSector(sector_number, sector))
            {
                return false;
            }
            loaded_sector = sector_number;
        }
        const uint8_t marker = sector[(index % EntriesPerSector) * RootEntrySize];
        if (marker == EndOfDirectory || marker == DeletedEntry)
        {
            run_start = run_length == 0 ? index : run_start;
            if (++run_length == count)
            {
                *first = {directory, run_start};
                return true;
            }
        }
        else
        {
            run_length = 0;
        }
        ++index;
    }
    return false;
}

bool WriteRawEntry(Slot slot, const uint8_t* bytes)
{
    uint8_t sector[BytesPerSector];
    uint64_t sector_number;
    uint8_t* entry;
    if (!ReadSlot(slot, sector, &sector_number, &entry))
    {
        return false;
    }
    for (uint32_t index = 0; index < RootEntrySize; ++index)
    {
        entry[index] = bytes[index];
    }
    return BlockDevice::WriteSector(sector_number, sector);
}

bool WriteEntry(Slot slot, const uint8_t (&name)[11], uint8_t attributes, uint32_t first_cluster,
                uint8_t case_flags)
{
    uint8_t entry[RootEntrySize];
    for (uint32_t index = 0; index < RootEntrySize; ++index)
    {
        entry[index] = index < sizeof(name) ? name[index] : 0;
    }
    entry[11] = attributes;
    entry[12] = case_flags;
    WriteLittleEndian16(entry + 26, static_cast<uint16_t>(first_cluster));
    return WriteRawEntry(slot, entry);
}

bool ShortNameTaken(uint32_t directory, const uint8_t (&short_name)[11], bool* taken)
{
    *taken = false;
    const Lookup result =
        ScanDirectory(directory,
                      [&](const uint8_t* entry, Slot)
                      {
                          if (entry[0] == EndOfDirectory || entry[0] == DeletedEntry)
                          {
                              return false;
                          }
                          *taken = true;
                          for (uint32_t index = 0; index < 11; ++index)
                          {
                              *taken = *taken && entry[index] == short_name[index];
                          }
                          return *taken;
                      });
    return result != Lookup::Error;
}

// Writes name into directory: as a short entry when it fits 8.3 with one case per part, or as
// long-name entries followed by a generated, unique "BASIS~N" short entry.
bool CreateEntry(uint32_t directory, const char* name, uint8_t attributes, uint32_t first_cluster,
                 Slot* entry_slot)
{
    if (!FatNames::IsValidName(name))
    {
        return false;
    }
    uint8_t short_name[11];
    uint8_t case_flags = 0;
    uint32_t long_entries = 0;
    if (!FatNames::ToShortName(name, short_name, &case_flags))
    {
        long_entries = FatNames::LongEntryCount(name);
        uint8_t basis[11];
        FatNames::MakeBasis(name, basis);
        bool taken = true;
        for (uint32_t number = 1; taken && number < 10000; ++number)
        {
            for (uint32_t index = 0; index < sizeof(basis); ++index)
            {
                short_name[index] = basis[index];
            }
            FatNames::ApplyTail(short_name, number);
            if (!ShortNameTaken(directory, short_name, &taken))
            {
                return false;
            }
        }
        if (taken)
        {
            return false;
        }
    }

    Slot first;
    if (!FindFreeRun(directory, long_entries + 1, &first))
    {
        return false;
    }
    // A crash between these writes leaves long-name entries with no short entry after them, which
    // readers skip.
    const uint8_t checksum = FatNames::Checksum(short_name);
    uint8_t entry[RootEntrySize];
    for (uint32_t index = 0; index < long_entries; ++index)
    {
        FatNames::EncodeLongEntry(name, long_entries - index, long_entries, checksum, entry);
        if (!WriteRawEntry({directory, first.index + index}, entry))
        {
            return false;
        }
    }
    *entry_slot = {directory, first.index + long_entries};
    return WriteEntry(*entry_slot, short_name, attributes, first_cluster, case_flags);
}

bool MarkDeleted(uint32_t directory, uint32_t first_slot, uint32_t last_slot)
{
    for (uint32_t index = first_slot; index <= last_slot; ++index)
    {
        uint8_t sector[BytesPerSector];
        uint64_t sector_number;
        uint8_t* entry;
        if (!ReadSlot({directory, index}, sector, &sector_number, &entry))
        {
            return false;
        }
        entry[0] = DeletedEntry;
        if (!BlockDevice::WriteSector(sector_number, sector))
        {
            return false;
        }
    }
    return true;
}

} // namespace

bool Mount()
{
    mounted = false;
    Partition::Range partitions[Partition::MaximumPartitions];
    const uint32_t partition_count = Partition::Find(partitions);
    uint8_t boot_sector[BytesPerSector];
    for (uint32_t index = 0; index < partition_count; ++index)
    {
        if (MountPartition(partitions[index].first_sector, partitions[index].sector_count,
                           boot_sector))
        {
            return true;
        }
    }
    return false;
}

bool Open(const char* path, File* file)
{
    char buffer[MaximumPathLength];
    uint32_t directory;
    const char* leaf;
    Found found;
    if (!mounted || file == nullptr ||
        FindPath(path, buffer, &directory, &leaf, &found) != Lookup::Found ||
        (found.attributes & DirectoryAttribute) != 0)
    {
        return false;
    }
    *file = found.file;
    return true;
}

bool Stat(const char* path, FileInfo* info)
{
    File file;
    if (info == nullptr || !Open(path, &file))
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

bool Create(const char* path, File* file)
{
    char buffer[MaximumPathLength];
    uint32_t directory;
    const char* leaf;
    Found existing;
    Slot slot;
    if (!mounted || file == nullptr ||
        FindPath(path, buffer, &directory, &leaf, &existing) != Lookup::Missing ||
        !CreateEntry(directory, leaf, ArchiveAttribute, 0, &slot))
    {
        return false;
    }
    *file = {0, 0, slot.directory, slot.index};
    return true;
}

bool MakeDirectory(const char* path)
{
    char buffer[MaximumPathLength];
    uint32_t parent;
    const char* leaf;
    Found existing;
    uint32_t cluster;
    if (!mounted || FindPath(path, buffer, &parent, &leaf, &existing) != Lookup::Missing ||
        !FatNames::IsValidName(leaf) || !AllocateCluster(&cluster))
    {
        return false;
    }
    // The new cluster is zeroed, so only "." and ".." need writing before the parent links it.
    uint8_t dot[11];
    uint8_t dot_dot[11];
    for (uint32_t index = 0; index < sizeof(dot); ++index)
    {
        dot[index] = index == 0 ? '.' : ' ';
        dot_dot[index] = index < 2 ? '.' : ' ';
    }
    Slot slot;
    return WriteEntry({cluster, 0}, dot, DirectoryAttribute, cluster, 0) &&
           WriteEntry({cluster, 1}, dot_dot, DirectoryAttribute, parent, 0) &&
           CreateEntry(parent, leaf, DirectoryAttribute, cluster, &slot);
}

bool RemoveDirectory(const char* path)
{
    char buffer[MaximumPathLength];
    uint32_t parent;
    const char* leaf;
    Found found;
    if (!mounted || FindPath(path, buffer, &parent, &leaf, &found) != Lookup::Found ||
        (found.attributes & DirectoryAttribute) == 0 || FatNames::SameName(leaf, ".") ||
        FatNames::SameName(leaf, "..") || found.file.first_cluster == RootDirectory)
    {
        return false;
    }
    bool empty = true;
    const Lookup scan = ScanNames(found.file.first_cluster,
                                  [&](const char*, const uint8_t* entry, Slot, uint32_t)
                                  {
                                      empty = IsDotEntry(entry);
                                      return !empty;
                                  });
    return scan == Lookup::Missing && empty &&
           MarkDeleted(parent, found.first_slot, found.file.entry_index) &&
           FreeChain(found.file.first_cluster);
}

bool Write(File* file, uint64_t offset, const void* buffer, uint64_t length,
           uint64_t* bytes_written)
{
    if (bytes_written == nullptr)
    {
        return false;
    }
    *bytes_written = 0;
    return mounted && file != nullptr &&
           WriteFile(file, offset, static_cast<const uint8_t*>(buffer), length, bytes_written);
}

bool Truncate(File* file, uint64_t size)
{
    return mounted && file != nullptr && TruncateFile(file, size);
}

bool Remove(const char* path)
{
    char buffer[MaximumPathLength];
    uint32_t directory;
    const char* leaf;
    Found found;
    return mounted && FindPath(path, buffer, &directory, &leaf, &found) == Lookup::Found &&
           (found.attributes & DirectoryAttribute) == 0 &&
           MarkDeleted(directory, found.first_slot, found.file.entry_index) &&
           (found.file.first_cluster == 0 || FreeChain(found.file.first_cluster));
}

bool IsDirectory(const char* path)
{
    uint32_t directory;
    return mounted && path != nullptr && ResolveDirectory(path, &directory);
}

bool ReadDirectory(const char* path, uint32_t index, DirectoryEntry* entry)
{
    uint32_t directory;
    if (!mounted || entry == nullptr || !ResolveDirectory(path, &directory))
    {
        return false;
    }
    uint32_t listed = 0;
    return ScanNames(directory,
                     [&](const char* name, const uint8_t* raw, Slot, uint32_t)
                     {
                         if (IsDotEntry(raw) || listed++ != index)
                         {
                             return false;
                         }
                         for (uint32_t offset = 0; offset <= FatNames::MaximumNameLength; ++offset)
                         {
                             entry->name[offset] = name[offset];
                             if (name[offset] == '\0')
                             {
                                 break;
                             }
                         }
                         entry->directory = (raw[11] & DirectoryAttribute) != 0;
                         entry->size = ReadLittleEndian32(raw + 28);
                         return true;
                     }) == Lookup::Found;
}

uint32_t FreeClusterCount()
{
    if (!mounted)
    {
        return 0;
    }
    uint32_t free_clusters = 0;
    uint8_t sector[BytesPerSector];
    for (uint32_t fat_sector = 0; fat_sector < fat_size_sectors; ++fat_sector)
    {
        if (!BlockDevice::ReadSector(partition_start + reserved_sectors + fat_sector, sector))
        {
            return 0;
        }
        for (uint32_t slot = 0; slot < FatEntriesPerSector; ++slot)
        {
            const uint32_t cluster = fat_sector * FatEntriesPerSector + slot;
            if (cluster >= 2 && IsDataCluster(cluster) &&
                ReadLittleEndian16(sector + slot * 2) == Fat16FreeCluster)
            {
                ++free_clusters;
            }
        }
    }
    return free_clusters;
}

} // namespace FeatherOS::FAT16