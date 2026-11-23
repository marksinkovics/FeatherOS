// SPDX-License-Identifier: MIT OR Apache-2.0
#include "fs/fat.h"

#include "common/little_endian.h"
#include "fat/names.h"
#include "fs/host.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace FeatherDisk::Fat
{
namespace
{

constexpr std::uint32_t BytesPerSector = 512;
constexpr std::uint16_t BootSignature = 0xAA55;
// FAT12 below this many clusters, FAT32 from the upper bound.
constexpr std::uint32_t MinimumClusters = 4085;
constexpr std::uint32_t MaximumClusters = 65525;
constexpr std::uint16_t Fat16CleanShutdownBit = 0x8000;
constexpr std::uint16_t Fat16NoErrorBit = 0x4000;
constexpr std::uint16_t Fat16EndOfChain = 0xFFF8;
constexpr std::uint16_t Fat16EndOfChainMark = 0xFFFF;
constexpr std::uint16_t Fat16FreeCluster = 0;
constexpr std::uint32_t EntriesPerSector = BytesPerSector / EntrySize;
constexpr std::uint32_t FatEntriesPerSector = BytesPerSector / 2;
constexpr std::uint8_t EndOfDirectory = 0x00;
constexpr std::uint8_t DeletedEntry = 0xE5;
constexpr std::uint8_t ArchiveAttribute = 0x20;
constexpr std::uint8_t DirectoryAttribute = 0x10;
constexpr std::uint8_t VolumeLabelAttribute = 0x08;
// Directory cluster 0 names the root directory, which sits in its own fixed region.
constexpr std::uint32_t RootDirectory = 0;
// FAT limits a directory to 65536 entries; the bound also stops a looping cluster chain.
constexpr std::uint32_t MaximumDirectoryEntries = 65536;
// Directory entries store sizes in 32 bits.
constexpr std::uint64_t MaximumFileSize = UINT32_MAX;
// Short-name aliases run from "~1" to "~9999".
constexpr std::uint32_t MaximumTail = 10000;
constexpr std::uint64_t PastTheEnd = UINT64_MAX;

// A file's node ID is where its directory entry is: its directory's first cluster (0 for the
// root) in the upper half and the entry's index there in the lower, so it changes only when a
// rename moves the entry. A directory's is its first cluster with DirectoryNode set, because that
// is all a ".." entry records of it; the root, which has no cluster, is cluster 0.
constexpr NodeId DirectoryNode = NodeId{1} << 63;
constexpr NodeId LowHalf = 0xFFFFFFFFU;

// FAT has no permissions, so nodes report what a Unix system gives new files: the owner may
// write, everyone may read, and directories may be searched.
constexpr std::uint32_t FileMode = 0644;
constexpr std::uint32_t DirectoryMode = 0755;
constexpr std::uint32_t WriteBits = 0222;

using Sector = std::array<std::uint8_t, BytesPerSector>;

constexpr Sector ZeroSector{};

std::uint16_t Load16(const std::uint8_t* bytes)
{
    return LoadLittleEndian<std::uint16_t>(bytes);
}

std::uint32_t Load32(const std::uint8_t* bytes)
{
    return LoadLittleEndian<std::uint32_t>(bytes);
}

template <typename T> std::unexpected<Error> Failure(const Result<T>& result)
{
    return std::unexpected(result.error());
}

struct Geometry
{
    std::uint32_t total_sectors;
    std::uint32_t reserved_sectors;
    std::uint32_t fat_count;
    std::uint32_t fat_size_sectors;
    std::uint32_t root_entry_count;
    std::uint32_t data_start_sector;
    std::uint32_t sectors_per_cluster;
    std::uint32_t cluster_count;
    std::uint32_t cluster_size;
};

std::optional<Geometry> ReadGeometry(const Sector& boot_sector, std::uint64_t sector_count)
{
    const std::uint8_t* boot = boot_sector.data();
    if (sector_count == 0 || Load16(boot + 510) != BootSignature ||
        Load16(boot + 11) != BytesPerSector)
    {
        return std::nullopt;
    }

    const std::uint32_t sectors_per_cluster = boot[13];
    const std::uint32_t reserved_sectors = Load16(boot + 14);
    const std::uint32_t fat_count = boot[16];
    const std::uint32_t root_entry_count = Load16(boot + 17);
    const std::uint32_t total_sectors_16 = Load16(boot + 19);
    const std::uint32_t fat_size = Load16(boot + 22);
    const std::uint32_t total_sectors =
        total_sectors_16 != 0 ? total_sectors_16 : Load32(boot + 32);
    if (sectors_per_cluster == 0 || (sectors_per_cluster & (sectors_per_cluster - 1)) != 0 ||
        reserved_sectors == 0 || fat_count == 0 || root_entry_count == 0 || fat_size == 0 ||
        total_sectors == 0 || total_sectors > sector_count)
    {
        return std::nullopt;
    }

    const std::uint32_t root_directory_sectors =
        (root_entry_count * EntrySize + BytesPerSector - 1) / BytesPerSector;
    const std::uint64_t metadata_sectors = std::uint64_t{reserved_sectors} +
                                           std::uint64_t{fat_count} * fat_size +
                                           root_directory_sectors;
    if (metadata_sectors >= total_sectors || metadata_sectors > UINT32_MAX)
    {
        return std::nullopt;
    }

    const auto cluster_count =
        static_cast<std::uint32_t>((total_sectors - metadata_sectors) / sectors_per_cluster);
    if (cluster_count < MinimumClusters || cluster_count >= MaximumClusters)
    {
        return std::nullopt;
    }
    return Geometry{
        .total_sectors = total_sectors,
        .reserved_sectors = reserved_sectors,
        .fat_count = fat_count,
        .fat_size_sectors = fat_size,
        .root_entry_count = root_entry_count,
        .data_start_sector = static_cast<std::uint32_t>(metadata_sectors),
        .sectors_per_cluster = sectors_per_cluster,
        .cluster_count = cluster_count,
        .cluster_size = BytesPerSector * sectors_per_cluster,
    };
}

Result<Geometry> ReadVolumeGeometry(BlockDevice& volume)
{
    if (volume.SectorSize() != BytesPerSector)
    {
        return std::unexpected(Error::Unsupported);
    }
    Sector boot_sector;
    if (const Status read = volume.Read(0, boot_sector); !read)
    {
        return Failure(read);
    }
    const std::optional<Geometry> geometry = ReadGeometry(boot_sector, volume.SectorCount());
    if (!geometry)
    {
        return std::unexpected(Error::Unsupported);
    }
    return *geometry;
}

// A directory entry's position: its directory's first cluster and its index there.
struct Slot
{
    std::uint32_t directory;
    std::uint32_t index;
};

struct File
{
    std::uint32_t first_cluster;
    std::uint32_t size;
    Slot slot;
};

struct Found
{
    File file;
    std::uint8_t attributes;
    // The first of the entry's long-name entries, or the entry itself without a long name.
    std::uint32_t first_slot;
};

NodeId FileNode(Slot slot)
{
    return (NodeId{slot.directory} << 32) | slot.index;
}

NodeId DirectoryNodeOf(std::uint32_t cluster)
{
    return DirectoryNode | cluster;
}

bool IsDirectoryNode(NodeId node)
{
    return (node & DirectoryNode) != 0;
}

bool IsDotName(std::string_view name)
{
    return name == "." || name == "..";
}

bool IsDotEntry(const std::uint8_t* entry)
{
    return entry[0] == '.' && (entry[1] == ' ' || (entry[1] == '.' && entry[2] == ' '));
}

class Fat16Filesystem final : public Filesystem
{
public:
    Fat16Filesystem(BlockDevice& device, const Geometry& layout, bool mounted_read_only);
    ~Fat16Filesystem() = default;

    // Constructed in memory from Host::Allocate; the core has no <new> for the global form.
    static void* operator new(std::size_t size, void* place) noexcept;

    Capabilities GetCapabilities() const override;
    NodeId Root() const override;
    Result<VolumeStatistics> Statistics() override;
    Result<NodeId> Lookup(NodeId directory, std::string_view name) override;
    Result<bool> ReadDirectory(NodeId directory, std::uint64_t& cookie,
                               DirectoryEntry& entry) override;
    Result<Attributes> GetAttributes(NodeId node) override;
    Result<std::size_t> Read(NodeId file, std::uint64_t offset,
                             std::span<std::uint8_t> buffer) override;
    Result<std::size_t> Write(NodeId file, std::uint64_t offset,
                              std::span<const std::uint8_t> data) override;
    Status Truncate(NodeId file, std::uint64_t size) override;
    Result<NodeId> Create(NodeId directory, std::string_view name, std::uint32_t mode) override;
    Result<NodeId> MakeDirectory(NodeId directory, std::string_view name,
                                 std::uint32_t mode) override;
    Status Remove(NodeId directory, std::string_view name) override;
    Status RemoveDirectory(NodeId directory, std::string_view name) override;
    Status Rename(NodeId from_directory, std::string_view from_name, NodeId to_directory,
                  std::string_view to_name) override;
    Status Sync() override;
    Status MakeReadOnly() override;

private:
    Status ReadSector(std::uint64_t sector, Sector& buffer);
    Status WriteSector(std::uint64_t sector, const Sector& buffer);
    Status Writable() const;
    bool IsDataCluster(std::uint32_t cluster) const;
    std::uint64_t FatSector(std::uint32_t copy, std::uint64_t fat_offset) const;
    std::uint64_t RootSector(std::uint32_t entry_index) const;
    std::uint64_t ClusterSector(std::uint32_t cluster) const;
    std::uint32_t EntriesPerCluster() const;
    Result<std::uint32_t> DirectoryCluster(NodeId directory) const;
    Result<File> LoadFile(NodeId node);

    Result<std::uint16_t> ReadFatEntry(std::uint32_t cluster);
    Status WriteFatEntry(std::uint32_t cluster, std::uint16_t value);
    Result<std::uint32_t> NextCluster(std::uint32_t cluster);
    Result<std::uint32_t> AllocateCluster();
    Status NextOrNewCluster(std::uint32_t& cluster);
    Status FreeChain(std::uint32_t first_cluster);
    Result<std::uint32_t> CountFreeClusters();

    Result<std::uint64_t> SlotSector(Slot slot);
    template <typename Visit>
    Result<bool> ScanDirectory(std::uint32_t directory, std::uint32_t start, Visit visit);
    template <typename Visit>
    Result<bool> ScanNames(std::uint32_t directory, std::uint32_t start, Visit visit);
    Result<bool> FindEntry(std::uint32_t directory, std::string_view target, Found& found);
    Result<std::uint64_t> ReadSlot(Slot slot, Sector& sector, std::uint8_t*& entry);
    Status UpdateEntry(const File& file);
    Status GrowDirectory(std::uint32_t directory);
    Status FindFreeRun(std::uint32_t directory, std::uint32_t count, Slot& first);
    Status WriteRawEntry(Slot slot, const std::uint8_t* bytes);
    Status WriteEntry(Slot slot, const ShortName& name, std::uint8_t attributes,
                      std::uint32_t first_cluster, std::uint8_t case_flags,
                      const std::uint8_t* model = nullptr);
    Result<bool> ShortNameTaken(std::uint32_t directory, const ShortName& short_name);
    Status CreateEntry(std::uint32_t directory, std::string_view name, std::uint8_t attributes,
                       std::uint32_t first_cluster, Slot& entry_slot,
                       const std::uint8_t* model = nullptr);
    Status MarkDeleted(std::uint32_t directory, std::uint32_t first_slot, std::uint32_t last_slot);
    Status RemoveEntry(const Found& found);
    Result<bool> IsEmptyDirectory(std::uint32_t cluster);
    Result<bool> OutsideOf(std::uint32_t directory, std::uint32_t cluster);
    Status SetParent(std::uint32_t directory, std::uint32_t parent);

    Result<std::size_t> ReadFile(const File& file, std::uint64_t offset,
                                 std::span<std::uint8_t> buffer);
    Result<std::size_t> WriteFile(File& file, std::uint64_t offset, const std::uint8_t* source,
                                  std::uint64_t length);
    Status ExtendFile(File& file, std::uint64_t size);
    Status ShrinkFile(File& file, std::uint64_t size);

    BlockDevice& volume;
    Geometry geometry;
    bool read_only;
};

class Fat16FilesystemType final : public FilesystemType
{
public:
    constexpr Fat16FilesystemType() = default;

    std::string_view Name() const override;
    bool Probe(BlockDevice& volume) override;
    Result<Filesystem*> Mount(BlockDevice& volume, MountOptions options) override;
    Status Unmount(Filesystem& filesystem) override;
    void Discard(Filesystem& filesystem) override;
};

Fat16Filesystem::Fat16Filesystem(BlockDevice& device, const Geometry& layout,
                                 bool mounted_read_only)
    : volume(device), geometry(layout), read_only(mounted_read_only)
{
}

void* Fat16Filesystem::operator new(std::size_t /*size*/, void* place) noexcept
{
    return place;
}

Status Fat16Filesystem::ReadSector(std::uint64_t sector, Sector& buffer)
{
    return volume.Read(sector, buffer);
}

Status Fat16Filesystem::WriteSector(std::uint64_t sector, const Sector& buffer)
{
    return volume.Write(sector, buffer);
}

Status Fat16Filesystem::Writable() const
{
    if (read_only)
    {
        return std::unexpected(Error::ReadOnly);
    }
    return {};
}

bool Fat16Filesystem::IsDataCluster(std::uint32_t cluster) const
{
    return cluster >= 2 && cluster < geometry.cluster_count + 2;
}

std::uint64_t Fat16Filesystem::FatSector(std::uint32_t copy, std::uint64_t fat_offset) const
{
    return geometry.reserved_sectors + std::uint64_t{copy} * geometry.fat_size_sectors +
           fat_offset / BytesPerSector;
}

std::uint64_t Fat16Filesystem::RootSector(std::uint32_t entry_index) const
{
    return geometry.reserved_sectors +
           std::uint64_t{geometry.fat_count} * geometry.fat_size_sectors +
           entry_index / EntriesPerSector;
}

std::uint64_t Fat16Filesystem::ClusterSector(std::uint32_t cluster) const
{
    return geometry.data_start_sector + std::uint64_t{cluster - 2} * geometry.sectors_per_cluster;
}

std::uint32_t Fat16Filesystem::EntriesPerCluster() const
{
    return EntriesPerSector * geometry.sectors_per_cluster;
}

Result<std::uint32_t> Fat16Filesystem::DirectoryCluster(NodeId directory) const
{
    if (!IsDirectoryNode(directory))
    {
        return std::unexpected(Error::NotDirectory);
    }
    const NodeId cluster = directory & ~DirectoryNode;
    if (cluster != RootDirectory &&
        (cluster > LowHalf || !IsDataCluster(static_cast<std::uint32_t>(cluster))))
    {
        return std::unexpected(Error::InvalidArgument);
    }
    return static_cast<std::uint32_t>(cluster);
}

Result<File> Fat16Filesystem::LoadFile(NodeId node)
{
    if (IsDirectoryNode(node))
    {
        return std::unexpected(Error::IsDirectory);
    }
    const Slot slot = {static_cast<std::uint32_t>(node >> 32),
                       static_cast<std::uint32_t>(node & LowHalf)};
    if (slot.directory != RootDirectory && !IsDataCluster(slot.directory))
    {
        return std::unexpected(Error::InvalidArgument);
    }
    Sector sector;
    std::uint8_t* entry = nullptr;
    if (const Result<std::uint64_t> read = ReadSlot(slot, sector, entry); !read)
    {
        return Failure(read);
    }
    if (entry[0] == EndOfDirectory || entry[0] == DeletedEntry || entry[11] == LongNameAttribute ||
        (entry[11] & VolumeLabelAttribute) != 0)
    {
        return std::unexpected(Error::NotFound);
    }
    if ((entry[11] & DirectoryAttribute) != 0)
    {
        return std::unexpected(Error::IsDirectory);
    }
    const File file = {Load16(entry + 26), Load32(entry + 28), slot};
    if (file.first_cluster == 0 ? file.size != 0 : !IsDataCluster(file.first_cluster))
    {
        return std::unexpected(Error::Corrupt);
    }
    return file;
}

Result<std::uint16_t> Fat16Filesystem::ReadFatEntry(std::uint32_t cluster)
{
    const std::uint64_t fat_offset = std::uint64_t{cluster} * 2;
    if (fat_offset / BytesPerSector >= geometry.fat_size_sectors)
    {
        return std::unexpected(Error::Corrupt);
    }
    Sector sector;
    if (const Status read = ReadSector(FatSector(0, fat_offset), sector); !read)
    {
        return Failure(read);
    }
    return Load16(sector.data() + fat_offset % BytesPerSector);
}

// Every FAT copy gets the same update, so disk checkers see them agree.
Status Fat16Filesystem::WriteFatEntry(std::uint32_t cluster, std::uint16_t value)
{
    const std::uint64_t fat_offset = std::uint64_t{cluster} * 2;
    if (fat_offset / BytesPerSector >= geometry.fat_size_sectors)
    {
        return std::unexpected(Error::Corrupt);
    }
    Sector sector;
    for (std::uint32_t copy = 0; copy < geometry.fat_count; ++copy)
    {
        const std::uint64_t sector_number = FatSector(copy, fat_offset);
        if (const Status read = ReadSector(sector_number, sector); !read)
        {
            return read;
        }
        StoreLittleEndian(sector.data() + fat_offset % BytesPerSector, value);
        if (const Status written = WriteSector(sector_number, sector); !written)
        {
            return written;
        }
    }
    return {};
}

// The cluster after cluster in its chain, or 0 where the chain ends.
Result<std::uint32_t> Fat16Filesystem::NextCluster(std::uint32_t cluster)
{
    const Result<std::uint16_t> next = ReadFatEntry(cluster);
    if (!next)
    {
        return Failure(next);
    }
    if (*next >= Fat16EndOfChain)
    {
        return 0U;
    }
    if (!IsDataCluster(*next))
    {
        return std::unexpected(Error::Corrupt);
    }
    return std::uint32_t{*next};
}

// New clusters are zeroed so a file never exposes a deleted file's old contents.
Result<std::uint32_t> Fat16Filesystem::AllocateCluster()
{
    Sector sector;
    for (std::uint32_t fat_sector = 0; fat_sector < geometry.fat_size_sectors; ++fat_sector)
    {
        if (const Status read = ReadSector(geometry.reserved_sectors + fat_sector, sector); !read)
        {
            return Failure(read);
        }
        for (std::uint32_t slot = 0; slot < FatEntriesPerSector; ++slot)
        {
            const std::uint32_t candidate = fat_sector * FatEntriesPerSector + slot;
            if (candidate < 2)
            {
                continue;
            }
            if (!IsDataCluster(candidate))
            {
                return std::unexpected(Error::NoSpace);
            }
            if (Load16(sector.data() + std::size_t{slot} * 2) != Fat16FreeCluster)
            {
                continue;
            }
            for (std::uint32_t index = 0; index < geometry.sectors_per_cluster; ++index)
            {
                if (const Status written =
                        WriteSector(ClusterSector(candidate) + index, ZeroSector);
                    !written)
                {
                    return Failure(written);
                }
            }
            if (const Status linked = WriteFatEntry(candidate, Fat16EndOfChainMark); !linked)
            {
                return Failure(linked);
            }
            return candidate;
        }
    }
    return std::unexpected(Error::NoSpace);
}

// Follows the chain to its next cluster, adding one at the end if the chain stops here.
Status Fat16Filesystem::NextOrNewCluster(std::uint32_t& cluster)
{
    const Result<std::uint16_t> next = ReadFatEntry(cluster);
    if (!next)
    {
        return Failure(next);
    }
    if (*next >= Fat16EndOfChain)
    {
        const Result<std::uint32_t> fresh = AllocateCluster();
        if (!fresh)
        {
            return Failure(fresh);
        }
        if (const Status linked = WriteFatEntry(cluster, static_cast<std::uint16_t>(*fresh));
            !linked)
        {
            return linked;
        }
        cluster = *fresh;
        return {};
    }
    if (!IsDataCluster(*next))
    {
        return std::unexpected(Error::Corrupt);
    }
    cluster = *next;
    return {};
}

// A chain longer than the volume has clusters must contain a loop.
Status Fat16Filesystem::FreeChain(std::uint32_t first_cluster)
{
    std::uint32_t cluster = first_cluster;
    for (std::uint32_t steps = 0; steps < geometry.cluster_count; ++steps)
    {
        if (!IsDataCluster(cluster))
        {
            return std::unexpected(Error::Corrupt);
        }
        const Result<std::uint16_t> next = ReadFatEntry(cluster);
        if (!next)
        {
            return Failure(next);
        }
        if (const Status freed = WriteFatEntry(cluster, Fat16FreeCluster); !freed)
        {
            return freed;
        }
        if (*next >= Fat16EndOfChain)
        {
            return {};
        }
        cluster = *next;
    }
    return std::unexpected(Error::Corrupt);
}

Result<std::uint32_t> Fat16Filesystem::CountFreeClusters()
{
    std::uint32_t free_clusters = 0;
    Sector sector;
    for (std::uint32_t fat_sector = 0; fat_sector < geometry.fat_size_sectors; ++fat_sector)
    {
        if (const Status read = ReadSector(geometry.reserved_sectors + fat_sector, sector); !read)
        {
            return Failure(read);
        }
        for (std::uint32_t slot = 0; slot < FatEntriesPerSector; ++slot)
        {
            const std::uint32_t cluster = fat_sector * FatEntriesPerSector + slot;
            if (IsDataCluster(cluster) &&
                Load16(sector.data() + std::size_t{slot} * 2) == Fat16FreeCluster)
            {
                ++free_clusters;
            }
        }
    }
    return free_clusters;
}

// PastTheEnd past the end of the directory, including where its cluster chain stops.
Result<std::uint64_t> Fat16Filesystem::SlotSector(Slot slot)
{
    if (slot.directory == RootDirectory)
    {
        if (slot.index >= geometry.root_entry_count)
        {
            return PastTheEnd;
        }
        return RootSector(slot.index);
    }
    if (slot.index >= MaximumDirectoryEntries)
    {
        return PastTheEnd;
    }
    std::uint32_t cluster = slot.directory;
    for (std::uint32_t step = 0; step < slot.index / EntriesPerCluster(); ++step)
    {
        const Result<std::uint32_t> next = NextCluster(cluster);
        if (!next)
        {
            return Failure(next);
        }
        if (*next == 0)
        {
            return PastTheEnd;
        }
        cluster = *next;
    }
    return ClusterSector(cluster) + (slot.index % EntriesPerCluster()) / EntriesPerSector;
}

// Calls visit(entry, slot) on each slot from start in order until it returns true (true) or the
// directory ends (false). The end-of-directory slot is visited too, so callers can claim it.
template <typename Visit>
Result<bool> Fat16Filesystem::ScanDirectory(std::uint32_t directory, std::uint32_t start,
                                            Visit visit)
{
    Sector sector;
    std::uint64_t loaded_sector = PastTheEnd;
    for (std::uint32_t index = start;; ++index)
    {
        const Result<std::uint64_t> sector_number = SlotSector({directory, index});
        if (!sector_number)
        {
            return Failure(sector_number);
        }
        if (*sector_number == PastTheEnd)
        {
            return false;
        }
        if (*sector_number != loaded_sector)
        {
            if (const Status read = ReadSector(*sector_number, sector); !read)
            {
                return Failure(read);
            }
            loaded_sector = *sector_number;
        }
        const std::uint8_t* entry =
            sector.data() + std::size_t{index % EntriesPerSector} * EntrySize;
        if (visit(entry, Slot{directory, index}))
        {
            return true;
        }
        if (entry[0] == EndOfDirectory)
        {
            return false;
        }
    }
}

// Calls visit(name, entry, slot, first_slot) for each file or directory entry, "." and ".."
// included. name is the long name when a valid one precedes the entry, otherwise the short one,
// and first_slot is where the entry's long-name entries start.
template <typename Visit>
Result<bool> Fat16Filesystem::ScanNames(std::uint32_t directory, std::uint32_t start, Visit visit)
{
    LongNameReader reader;
    reader.Reset();
    return ScanDirectory(
        directory, start,
        [&](const std::uint8_t* entry, Slot slot)
        {
            if (entry[0] == EndOfDirectory || entry[0] == DeletedEntry ||
                (entry[11] != LongNameAttribute && (entry[11] & VolumeLabelAttribute) != 0))
            {
                reader.Reset();
                return false;
            }
            if (entry[11] == LongNameAttribute)
            {
                reader.Feed(entry, slot.index);
                return false;
            }
            Name name{};
            std::uint32_t first_slot = slot.index;
            if (!reader.Finish(entry, name, first_slot))
            {
                ShortNameText(entry, name);
            }
            reader.Reset();
            return visit(name.View(), entry, slot, first_slot);
        });
}

// A name matches an entry's long name or its short name, ignoring case.
Result<bool> Fat16Filesystem::FindEntry(std::uint32_t directory, std::string_view target,
                                        Found& found)
{
    bool valid = true;
    const Result<bool> result = ScanNames(
        directory, 0,
        [&](std::string_view name, const std::uint8_t* entry, Slot slot, std::uint32_t first_slot)
        {
            Name short_name{};
            ShortNameText(entry, short_name);
            if (!SameName(name, target) && !SameName(short_name.View(), target))
            {
                return false;
            }
            File& file = found.file;
            file.first_cluster = Load16(entry + 26);
            file.size = Load32(entry + 28);
            file.slot = slot;
            found.attributes = entry[11];
            found.first_slot = first_slot;
            // A ".." entry that points at the root records cluster 0.
            valid = (entry[11] & DirectoryAttribute) != 0
                        ? file.first_cluster == RootDirectory || IsDataCluster(file.first_cluster)
                        : (file.first_cluster == 0 ? file.size == 0
                                                   : IsDataCluster(file.first_cluster));
            return true;
        });
    if (result && *result && !valid)
    {
        return std::unexpected(Error::Corrupt);
    }
    return result;
}

Result<std::uint64_t> Fat16Filesystem::ReadSlot(Slot slot, Sector& sector, std::uint8_t*& entry)
{
    const Result<std::uint64_t> sector_number = SlotSector(slot);
    if (!sector_number)
    {
        return Failure(sector_number);
    }
    if (*sector_number == PastTheEnd)
    {
        return std::unexpected(Error::NotFound);
    }
    if (const Status read = ReadSector(*sector_number, sector); !read)
    {
        return Failure(read);
    }
    entry = sector.data() + std::size_t{slot.index % EntriesPerSector} * EntrySize;
    return *sector_number;
}

Status Fat16Filesystem::UpdateEntry(const File& file)
{
    Sector sector;
    std::uint8_t* entry = nullptr;
    const Result<std::uint64_t> sector_number = ReadSlot(file.slot, sector, entry);
    if (!sector_number)
    {
        return Failure(sector_number);
    }
    StoreLittleEndian(entry + 26, static_cast<std::uint16_t>(file.first_cluster));
    StoreLittleEndian(entry + 28, file.size);
    return WriteSector(*sector_number, sector);
}

// Adds a zeroed cluster, whose entries are all free, to the end of a subdirectory.
Status Fat16Filesystem::GrowDirectory(std::uint32_t directory)
{
    if (directory == RootDirectory)
    {
        return std::unexpected(Error::NoSpace);
    }
    std::uint32_t cluster = directory;
    std::uint32_t clusters = 1;
    for (;;)
    {
        const Result<std::uint32_t> next = NextCluster(cluster);
        if (!next)
        {
            return Failure(next);
        }
        if (*next == 0)
        {
            break;
        }
        if (++clusters >= MaximumDirectoryEntries / EntriesPerCluster())
        {
            return std::unexpected(Error::NoSpace);
        }
        cluster = *next;
    }
    return NextOrNewCluster(cluster);
}

// Finds count consecutive free slots, growing a subdirectory when it runs out.
Status Fat16Filesystem::FindFreeRun(std::uint32_t directory, std::uint32_t count, Slot& first)
{
    Sector sector;
    std::uint64_t loaded_sector = PastTheEnd;
    std::uint32_t run_start = 0;
    std::uint32_t run_length = 0;
    for (std::uint32_t index = 0; index < MaximumDirectoryEntries;)
    {
        const Result<std::uint64_t> sector_number = SlotSector({directory, index});
        if (!sector_number)
        {
            return Failure(sector_number);
        }
        if (*sector_number == PastTheEnd)
        {
            if (const Status grown = GrowDirectory(directory); !grown)
            {
                return grown;
            }
            continue;
        }
        if (*sector_number != loaded_sector)
        {
            if (const Status read = ReadSector(*sector_number, sector); !read)
            {
                return read;
            }
            loaded_sector = *sector_number;
        }
        const std::uint8_t marker = sector[std::size_t{index % EntriesPerSector} * EntrySize];
        if (marker == EndOfDirectory || marker == DeletedEntry)
        {
            run_start = run_length == 0 ? index : run_start;
            if (++run_length == count)
            {
                first = {directory, run_start};
                return {};
            }
        }
        else
        {
            run_length = 0;
        }
        ++index;
    }
    return std::unexpected(Error::NoSpace);
}

Status Fat16Filesystem::WriteRawEntry(Slot slot, const std::uint8_t* bytes)
{
    Sector sector;
    std::uint8_t* entry = nullptr;
    const Result<std::uint64_t> sector_number = ReadSlot(slot, sector, entry);
    if (!sector_number)
    {
        return Failure(sector_number);
    }
    for (std::uint32_t index = 0; index < EntrySize; ++index)
    {
        entry[index] = bytes[index];
    }
    return WriteSector(*sector_number, sector);
}

// The bytes after the name and flags (times and size) come from model when there is one.
Status Fat16Filesystem::WriteEntry(Slot slot, const ShortName& name, std::uint8_t attributes,
                                   std::uint32_t first_cluster, std::uint8_t case_flags,
                                   const std::uint8_t* model)
{
    std::array<std::uint8_t, EntrySize> entry{};
    for (std::uint32_t index = 0; index < EntrySize; ++index)
    {
        entry[index] = index < name.size() ? name[index] : (model != nullptr ? model[index] : 0);
    }
    entry[11] = attributes;
    entry[12] = case_flags;
    StoreLittleEndian(entry.data() + 26, static_cast<std::uint16_t>(first_cluster));
    return WriteRawEntry(slot, entry.data());
}

Result<bool> Fat16Filesystem::ShortNameTaken(std::uint32_t directory, const ShortName& short_name)
{
    return ScanDirectory(directory, 0,
                         [&](const std::uint8_t* entry, Slot /*slot*/)
                         {
                             if (entry[0] == EndOfDirectory || entry[0] == DeletedEntry)
                             {
                                 return false;
                             }
                             for (std::uint32_t index = 0; index < short_name.size(); ++index)
                             {
                                 if (entry[index] != short_name[index])
                                 {
                                     return false;
                                 }
                             }
                             return true;
                         });
}

// Writes name into directory: as a short entry when it fits 8.3 with one case per part, or as
// long-name entries followed by a generated, unique "BASIS~N" short entry.
Status Fat16Filesystem::CreateEntry(std::uint32_t directory, std::string_view name,
                                    std::uint8_t attributes, std::uint32_t first_cluster,
                                    Slot& entry_slot, const std::uint8_t* model)
{
    if (name.size() > MaximumNameLength)
    {
        return std::unexpected(Error::NameTooLong);
    }
    if (!IsValidName(name))
    {
        return std::unexpected(Error::InvalidArgument);
    }
    ShortName short_name{};
    std::uint8_t case_flags = 0;
    std::uint32_t long_entries = 0;
    if (!ToShortName(name, short_name, case_flags))
    {
        long_entries = LongEntryCount(name);
        ShortName basis{};
        MakeBasis(name, basis);
        bool taken = true;
        for (std::uint32_t number = 1; taken && number < MaximumTail; ++number)
        {
            short_name = basis;
            ApplyTail(short_name, number);
            const Result<bool> found = ShortNameTaken(directory, short_name);
            if (!found)
            {
                return Failure(found);
            }
            taken = *found;
        }
        if (taken)
        {
            return std::unexpected(Error::Exists);
        }
    }

    Slot first{};
    if (const Status found = FindFreeRun(directory, long_entries + 1, first); !found)
    {
        return found;
    }
    // A crash between these writes leaves long-name entries with no short entry after them, which
    // readers skip.
    const std::uint8_t checksum = Checksum(short_name.data());
    std::array<std::uint8_t, EntrySize> entry{};
    for (std::uint32_t index = 0; index < long_entries; ++index)
    {
        EncodeLongEntry(name, long_entries - index, long_entries, checksum, entry.data());
        if (const Status written = WriteRawEntry({directory, first.index + index}, entry.data());
            !written)
        {
            return written;
        }
    }
    entry_slot = {directory, first.index + long_entries};
    return WriteEntry(entry_slot, short_name, attributes, first_cluster, case_flags, model);
}

Status Fat16Filesystem::MarkDeleted(std::uint32_t directory, std::uint32_t first_slot,
                                    std::uint32_t last_slot)
{
    for (std::uint32_t index = first_slot; index <= last_slot; ++index)
    {
        Sector sector;
        std::uint8_t* entry = nullptr;
        const Result<std::uint64_t> sector_number = ReadSlot({directory, index}, sector, entry);
        if (!sector_number)
        {
            return Failure(sector_number);
        }
        entry[0] = DeletedEntry;
        if (const Status written = WriteSector(*sector_number, sector); !written)
        {
            return written;
        }
    }
    return {};
}

// The entry goes before its clusters, so an interruption can only leak them.
Status Fat16Filesystem::RemoveEntry(const Found& found)
{
    if (const Status deleted =
            MarkDeleted(found.file.slot.directory, found.first_slot, found.file.slot.index);
        !deleted)
    {
        return deleted;
    }
    return found.file.first_cluster == 0 ? Status{} : FreeChain(found.file.first_cluster);
}

Result<bool> Fat16Filesystem::IsEmptyDirectory(std::uint32_t cluster)
{
    const Result<bool> other =
        ScanNames(cluster, 0, [](std::string_view, const std::uint8_t* entry, Slot, std::uint32_t)
                  { return !IsDotEntry(entry); });
    if (!other)
    {
        return Failure(other);
    }
    return !*other;
}

// False when directory is cluster or lies inside it. Each step up reaches another directory, so
// a walk longer than the volume has clusters has met a loop.
Result<bool> Fat16Filesystem::OutsideOf(std::uint32_t directory, std::uint32_t cluster)
{
    for (std::uint32_t depth = 0; depth <= geometry.cluster_count; ++depth)
    {
        if (directory == cluster)
        {
            return false;
        }
        if (directory == RootDirectory)
        {
            return true;
        }
        Found parent{};
        const Result<bool> found = FindEntry(directory, "..", parent);
        if (!found)
        {
            return Failure(found);
        }
        if (!*found)
        {
            return false;
        }
        directory = parent.file.first_cluster;
    }
    return false;
}

Status Fat16Filesystem::SetParent(std::uint32_t directory, std::uint32_t parent)
{
    Found dot_dot{};
    const Result<bool> found = FindEntry(directory, "..", dot_dot);
    if (!found)
    {
        return Failure(found);
    }
    if (!*found)
    {
        return std::unexpected(Error::Corrupt);
    }
    Sector sector;
    std::uint8_t* entry = nullptr;
    const Result<std::uint64_t> sector_number = ReadSlot(dot_dot.file.slot, sector, entry);
    if (!sector_number)
    {
        return Failure(sector_number);
    }
    StoreLittleEndian(entry + 26, static_cast<std::uint16_t>(parent));
    return WriteSector(*sector_number, sector);
}

Result<std::size_t> Fat16Filesystem::ReadFile(const File& file, std::uint64_t offset,
                                              std::span<std::uint8_t> buffer)
{
    if (offset >= file.size)
    {
        return std::size_t{0};
    }
    std::uint64_t remaining = file.size - offset;
    if (remaining > buffer.size())
    {
        remaining = buffer.size();
    }
    if (remaining == 0)
    {
        return std::size_t{0};
    }

    std::uint32_t cluster = file.first_cluster;
    std::uint64_t cluster_offset = offset;
    while (cluster_offset >= geometry.cluster_size)
    {
        const Result<std::uint32_t> next = NextCluster(cluster);
        if (!next)
        {
            return Failure(next);
        }
        if (*next == 0)
        {
            return std::unexpected(Error::Corrupt);
        }
        cluster = *next;
        cluster_offset -= geometry.cluster_size;
    }

    Sector sector;
    std::uint64_t copied = 0;
    while (remaining > 0)
    {
        const auto offset_in_sector = static_cast<std::uint32_t>(cluster_offset % BytesPerSector);
        if (const Status read =
                ReadSector(ClusterSector(cluster) + cluster_offset / BytesPerSector, sector);
            !read)
        {
            return Failure(read);
        }
        std::uint64_t chunk = BytesPerSector - offset_in_sector;
        if (chunk > remaining)
        {
            chunk = remaining;
        }
        for (std::uint64_t index = 0; index < chunk; ++index)
        {
            buffer[copied + index] = sector[offset_in_sector + index];
        }
        copied += chunk;
        remaining -= chunk;
        cluster_offset += chunk;
        if (remaining > 0 && cluster_offset == geometry.cluster_size)
        {
            const Result<std::uint32_t> next = NextCluster(cluster);
            if (!next)
            {
                return Failure(next);
            }
            if (*next == 0)
            {
                return std::unexpected(Error::Corrupt);
            }
            cluster = *next;
            cluster_offset = 0;
        }
    }
    return static_cast<std::size_t>(copied);
}

// Writes from offset, which is at most the file's size; a null source writes zeros. Stops at the
// first failure and returns the bytes written before it, or the failure if there were none.
Result<std::size_t> Fat16Filesystem::WriteFile(File& file, std::uint64_t offset,
                                               const std::uint8_t* source, std::uint64_t length)
{
    if (offset > file.size)
    {
        return std::unexpected(Error::InvalidArgument);
    }
    if (length > MaximumFileSize - offset)
    {
        return std::unexpected(Error::FileTooLarge);
    }
    if (length == 0)
    {
        return std::size_t{0};
    }
    if (file.first_cluster == 0)
    {
        const Result<std::uint32_t> first_cluster = AllocateCluster();
        if (!first_cluster)
        {
            return Failure(first_cluster);
        }
        file.first_cluster = *first_cluster;
        if (const Status updated = UpdateEntry(file); !updated)
        {
            return Failure(updated);
        }
    }

    std::uint32_t cluster = file.first_cluster;
    std::uint64_t cluster_number = 0;
    std::uint64_t position = offset;
    std::uint64_t written = 0;
    Status failure;
    Sector sector;
    while (written < length)
    {
        for (; cluster_number < position / geometry.cluster_size; ++cluster_number)
        {
            failure = NextOrNewCluster(cluster);
            if (!failure)
            {
                break;
            }
        }
        if (!failure)
        {
            break;
        }
        const std::uint64_t cluster_offset = position % geometry.cluster_size;
        const auto offset_in_sector = static_cast<std::uint32_t>(cluster_offset % BytesPerSector);
        const std::uint64_t sector_number =
            ClusterSector(cluster) + cluster_offset / BytesPerSector;
        std::uint64_t chunk = BytesPerSector - offset_in_sector;
        if (chunk > length - written)
        {
            chunk = length - written;
        }
        // A partial sector keeps the bytes around the new data.
        if (chunk < BytesPerSector)
        {
            failure = ReadSector(sector_number, sector);
            if (!failure)
            {
                break;
            }
        }
        for (std::uint64_t index = 0; index < chunk; ++index)
        {
            sector[offset_in_sector + index] = source == nullptr ? 0 : source[written + index];
        }
        failure = WriteSector(sector_number, sector);
        if (!failure)
        {
            break;
        }
        written += chunk;
        position += chunk;
    }
    if (position > file.size)
    {
        file.size = static_cast<std::uint32_t>(position);
        if (const Status updated = UpdateEntry(file); !updated)
        {
            return Failure(updated);
        }
    }
    if (written == 0 && !failure)
    {
        return Failure(failure);
    }
    return static_cast<std::size_t>(written);
}

// Fills the file with zeros from its end to size.
Status Fat16Filesystem::ExtendFile(File& file, std::uint64_t size)
{
    if (size > MaximumFileSize)
    {
        return std::unexpected(Error::FileTooLarge);
    }
    const std::uint64_t gap = size - file.size;
    const Result<std::size_t> written = WriteFile(file, file.size, nullptr, gap);
    if (!written)
    {
        return Failure(written);
    }
    if (*written != gap)
    {
        return std::unexpected(Error::NoSpace);
    }
    return {};
}

Status Fat16Filesystem::ShrinkFile(File& file, std::uint64_t size)
{
    if (size >= file.size)
    {
        return {};
    }
    const std::uint32_t old_first_cluster = file.first_cluster;
    const std::uint64_t kept_clusters = (size + geometry.cluster_size - 1) / geometry.cluster_size;
    if (kept_clusters == 0)
    {
        file.first_cluster = 0;
        file.size = 0;
        if (const Status updated = UpdateEntry(file); !updated)
        {
            return updated;
        }
        return old_first_cluster == 0 ? Status{} : FreeChain(old_first_cluster);
    }

    std::uint32_t last_cluster = old_first_cluster;
    for (std::uint64_t index = 1; index < kept_clusters; ++index)
    {
        const Result<std::uint32_t> next = NextCluster(last_cluster);
        if (!next)
        {
            return Failure(next);
        }
        if (*next == 0)
        {
            return std::unexpected(Error::Corrupt);
        }
        last_cluster = *next;
    }
    const Result<std::uint16_t> next = ReadFatEntry(last_cluster);
    if (!next)
    {
        return Failure(next);
    }
    file.size = static_cast<std::uint32_t>(size);
    if (const Status updated = UpdateEntry(file); !updated)
    {
        return updated;
    }
    if (*next >= Fat16EndOfChain)
    {
        return {};
    }
    if (const Status ended = WriteFatEntry(last_cluster, Fat16EndOfChainMark); !ended)
    {
        return ended;
    }
    return FreeChain(*next);
}

Capabilities Fat16Filesystem::GetCapabilities() const
{
    return {
        .writable = !read_only,
        .symbolic_links = false,
        .hard_links = false,
        .permissions = false,
        .case_sensitive = false,
        .maximum_name_length = MaximumNameLength,
        .maximum_file_size = MaximumFileSize,
    };
}

NodeId Fat16Filesystem::Root() const
{
    return DirectoryNodeOf(RootDirectory);
}

Result<VolumeStatistics> Fat16Filesystem::Statistics()
{
    const Result<std::uint32_t> free_clusters = CountFreeClusters();
    if (!free_clusters)
    {
        return Failure(free_clusters);
    }
    return VolumeStatistics{
        .block_size = geometry.cluster_size,
        .total_blocks = geometry.cluster_count,
        .free_blocks = *free_clusters,
        .total_nodes = 0,
        .free_nodes = 0,
    };
}

Result<NodeId> Fat16Filesystem::Lookup(NodeId directory, std::string_view name)
{
    const Result<std::uint32_t> cluster = DirectoryCluster(directory);
    if (!cluster)
    {
        return Failure(cluster);
    }
    // "." stays, and ".." in the root, which has neither entry, stays in the root.
    if (name == "." || (name == ".." && *cluster == RootDirectory))
    {
        return DirectoryNodeOf(*cluster);
    }
    if (name.size() > MaximumNameLength)
    {
        return std::unexpected(Error::NameTooLong);
    }
    Found found{};
    const Result<bool> result = FindEntry(*cluster, name, found);
    if (!result)
    {
        return Failure(result);
    }
    if (!*result)
    {
        return std::unexpected(Error::NotFound);
    }
    return (found.attributes & DirectoryAttribute) != 0 ? DirectoryNodeOf(found.file.first_cluster)
                                                        : FileNode(found.file.slot);
}

Result<bool> Fat16Filesystem::ReadDirectory(NodeId directory, std::uint64_t& cookie,
                                            DirectoryEntry& entry)
{
    const Result<std::uint32_t> cluster = DirectoryCluster(directory);
    if (!cluster)
    {
        return Failure(cluster);
    }
    if (cookie >= MaximumDirectoryEntries)
    {
        return false;
    }
    return ScanNames(*cluster, static_cast<std::uint32_t>(cookie),
                     [&](std::string_view name, const std::uint8_t* raw, Slot slot, std::uint32_t)
                     {
                         if (IsDotEntry(raw))
                         {
                             return false;
                         }
                         const bool is_directory = (raw[11] & DirectoryAttribute) != 0;
                         entry.node =
                             is_directory ? DirectoryNodeOf(Load16(raw + 26)) : FileNode(slot);
                         entry.type = is_directory ? NodeType::Directory : NodeType::File;
                         entry.name_length = static_cast<std::uint16_t>(name.size());
                         for (std::size_t index = 0; index < name.size(); ++index)
                         {
                             entry.name[index] = name[index];
                         }
                         cookie = std::uint64_t{slot.index} + 1;
                         return true;
                     });
}

Result<Attributes> Fat16Filesystem::GetAttributes(NodeId node)
{
    const std::uint32_t denied = read_only ? WriteBits : 0;
    Attributes attributes = {
        .node = node,
        .type = NodeType::File,
        .size = 0,
        .mode = FileMode & ~denied,
        .owner = 0,
        .group = 0,
        .link_count = 1,
        .access_time = {},
        .modify_time = {},
        .change_time = {},
    };
    if (IsDirectoryNode(node))
    {
        if (const Result<std::uint32_t> cluster = DirectoryCluster(node); !cluster)
        {
            return Failure(cluster);
        }
        attributes.type = NodeType::Directory;
        attributes.mode = DirectoryMode & ~denied;
        return attributes;
    }
    const Result<File> file = LoadFile(node);
    if (!file)
    {
        return Failure(file);
    }
    attributes.size = file->size;
    return attributes;
}

Result<std::size_t> Fat16Filesystem::Read(NodeId file, std::uint64_t offset,
                                          std::span<std::uint8_t> buffer)
{
    const Result<File> loaded = LoadFile(file);
    if (!loaded)
    {
        return Failure(loaded);
    }
    return ReadFile(*loaded, offset, buffer);
}

Result<std::size_t> Fat16Filesystem::Write(NodeId file, std::uint64_t offset,
                                           std::span<const std::uint8_t> data)
{
    if (const Status writable = Writable(); !writable)
    {
        return Failure(writable);
    }
    Result<File> loaded = LoadFile(file);
    if (!loaded)
    {
        return Failure(loaded);
    }
    if (offset > loaded->size)
    {
        if (const Status extended = ExtendFile(*loaded, offset); !extended)
        {
            return Failure(extended);
        }
    }
    return WriteFile(*loaded, offset, data.data(), data.size());
}

Status Fat16Filesystem::Truncate(NodeId file, std::uint64_t size)
{
    if (const Status writable = Writable(); !writable)
    {
        return writable;
    }
    Result<File> loaded = LoadFile(file);
    if (!loaded)
    {
        return Failure(loaded);
    }
    return size > loaded->size ? ExtendFile(*loaded, size) : ShrinkFile(*loaded, size);
}

Result<NodeId> Fat16Filesystem::Create(NodeId directory, std::string_view name,
                                       std::uint32_t /*mode*/)
{
    if (const Status writable = Writable(); !writable)
    {
        return Failure(writable);
    }
    const Result<std::uint32_t> cluster = DirectoryCluster(directory);
    if (!cluster)
    {
        return Failure(cluster);
    }
    Found existing{};
    const Result<bool> found = FindEntry(*cluster, name, existing);
    if (!found)
    {
        return Failure(found);
    }
    if (*found || IsDotName(name))
    {
        return std::unexpected(Error::Exists);
    }
    Slot slot{};
    if (const Status created = CreateEntry(*cluster, name, ArchiveAttribute, 0, slot); !created)
    {
        return Failure(created);
    }
    return FileNode(slot);
}

Result<NodeId> Fat16Filesystem::MakeDirectory(NodeId directory, std::string_view name,
                                              std::uint32_t /*mode*/)
{
    if (const Status writable = Writable(); !writable)
    {
        return Failure(writable);
    }
    const Result<std::uint32_t> parent = DirectoryCluster(directory);
    if (!parent)
    {
        return Failure(parent);
    }
    Found existing{};
    const Result<bool> found = FindEntry(*parent, name, existing);
    if (!found)
    {
        return Failure(found);
    }
    if (*found || IsDotName(name))
    {
        return std::unexpected(Error::Exists);
    }
    if (name.size() > MaximumNameLength)
    {
        return std::unexpected(Error::NameTooLong);
    }
    if (!IsValidName(name))
    {
        return std::unexpected(Error::InvalidArgument);
    }
    const Result<std::uint32_t> cluster = AllocateCluster();
    if (!cluster)
    {
        return Failure(cluster);
    }
    // The new cluster is zeroed, so only "." and ".." need writing before the parent links it.
    ShortName dot{};
    ShortName dot_dot{};
    dot.fill(' ');
    dot_dot.fill(' ');
    dot[0] = '.';
    dot_dot[0] = '.';
    dot_dot[1] = '.';
    Slot slot{};
    Status made = WriteEntry({*cluster, 0}, dot, DirectoryAttribute, *cluster, 0);
    if (made)
    {
        made = WriteEntry({*cluster, 1}, dot_dot, DirectoryAttribute, *parent, 0);
    }
    if (made)
    {
        made = CreateEntry(*parent, name, DirectoryAttribute, *cluster, slot);
    }
    if (!made)
    {
        // Nothing links the cluster yet, so it can go back; if that fails too, it only leaks.
        [[maybe_unused]] const Status freed = FreeChain(*cluster);
        return Failure(made);
    }
    return DirectoryNodeOf(*cluster);
}

Status Fat16Filesystem::Remove(NodeId directory, std::string_view name)
{
    if (const Status writable = Writable(); !writable)
    {
        return writable;
    }
    const Result<std::uint32_t> cluster = DirectoryCluster(directory);
    if (!cluster)
    {
        return Failure(cluster);
    }
    if (IsDotName(name))
    {
        return std::unexpected(Error::IsDirectory);
    }
    Found found{};
    const Result<bool> result = FindEntry(*cluster, name, found);
    if (!result)
    {
        return Failure(result);
    }
    if (!*result)
    {
        return std::unexpected(Error::NotFound);
    }
    if ((found.attributes & DirectoryAttribute) != 0)
    {
        return std::unexpected(Error::IsDirectory);
    }
    return RemoveEntry(found);
}

Status Fat16Filesystem::RemoveDirectory(NodeId directory, std::string_view name)
{
    if (const Status writable = Writable(); !writable)
    {
        return writable;
    }
    const Result<std::uint32_t> cluster = DirectoryCluster(directory);
    if (!cluster)
    {
        return Failure(cluster);
    }
    if (IsDotName(name))
    {
        return std::unexpected(Error::InvalidArgument);
    }
    Found found{};
    const Result<bool> result = FindEntry(*cluster, name, found);
    if (!result)
    {
        return Failure(result);
    }
    if (!*result)
    {
        return std::unexpected(Error::NotFound);
    }
    if ((found.attributes & DirectoryAttribute) == 0)
    {
        return std::unexpected(Error::NotDirectory);
    }
    if (found.file.first_cluster == RootDirectory)
    {
        return std::unexpected(Error::Corrupt);
    }
    const Result<bool> empty = IsEmptyDirectory(found.file.first_cluster);
    if (!empty)
    {
        return Failure(empty);
    }
    if (!*empty)
    {
        return std::unexpected(Error::NotEmpty);
    }
    return RemoveEntry(found);
}

Status Fat16Filesystem::Rename(NodeId from_directory, std::string_view from_name,
                               NodeId to_directory, std::string_view to_name)
{
    if (const Status writable = Writable(); !writable)
    {
        return writable;
    }
    const Result<std::uint32_t> from_cluster = DirectoryCluster(from_directory);
    if (!from_cluster)
    {
        return Failure(from_cluster);
    }
    const Result<std::uint32_t> to_cluster = DirectoryCluster(to_directory);
    if (!to_cluster)
    {
        return Failure(to_cluster);
    }
    if (IsDotName(from_name) || IsDotName(to_name))
    {
        return std::unexpected(Error::InvalidArgument);
    }
    Found source{};
    const Result<bool> source_found = FindEntry(*from_cluster, from_name, source);
    if (!source_found)
    {
        return Failure(source_found);
    }
    if (!*source_found)
    {
        return std::unexpected(Error::NotFound);
    }
    Found existing{};
    const Result<bool> target = FindEntry(*to_cluster, to_name, existing);
    if (!target)
    {
        return Failure(target);
    }
    const bool same_entry = *target && existing.file.slot.directory == source.file.slot.directory &&
                            existing.file.slot.index == source.file.slot.index;
    const bool directory = (source.attributes & DirectoryAttribute) != 0;
    if (to_name.size() > MaximumNameLength)
    {
        return std::unexpected(Error::NameTooLong);
    }
    if (!IsValidName(to_name))
    {
        return std::unexpected(Error::InvalidArgument);
    }
    if (directory)
    {
        const Result<bool> outside = OutsideOf(*to_cluster, source.file.first_cluster);
        if (!outside)
        {
            return Failure(outside);
        }
        if (!*outside)
        {
            return std::unexpected(Error::InvalidArgument);
        }
    }
    if (*target && !same_entry)
    {
        const bool target_directory = (existing.attributes & DirectoryAttribute) != 0;
        if (directory != target_directory)
        {
            return std::unexpected(directory ? Error::NotDirectory : Error::IsDirectory);
        }
        if (target_directory)
        {
            const Result<bool> empty = IsEmptyDirectory(existing.file.first_cluster);
            if (!empty)
            {
                return Failure(empty);
            }
            if (!*empty)
            {
                return std::unexpected(Error::NotEmpty);
            }
        }
        // The one non-atomic window of a replacing rename: a crash from here to CreateEntry
        // leaves the source's name, with its data, and no target. Creating the new entry first
        // would leave two entries over one cluster chain, which fsck.fat cannot tell apart.
        if (const Status removed = RemoveEntry(existing); !removed)
        {
            return removed;
        }
    }

    Sector sector;
    std::uint8_t* entry = nullptr;
    if (const Result<std::uint64_t> read = ReadSlot(source.file.slot, sector, entry); !read)
    {
        return Failure(read);
    }
    std::array<std::uint8_t, EntrySize> model{};
    for (std::uint32_t index = 0; index < EntrySize; ++index)
    {
        model[index] = entry[index];
    }
    Slot slot{};
    // A new case for the same name needs the old entry gone first, so a crash can leak the
    // file's clusters. Otherwise the new entry comes first, and a crash leaves both names.
    if (same_entry)
    {
        if (const Status deleted =
                MarkDeleted(*from_cluster, source.first_slot, source.file.slot.index);
            !deleted)
        {
            return deleted;
        }
        return CreateEntry(*to_cluster, to_name, source.attributes, source.file.first_cluster, slot,
                           model.data());
    }
    if (const Status created = CreateEntry(*to_cluster, to_name, source.attributes,
                                           source.file.first_cluster, slot, model.data());
        !created)
    {
        return created;
    }
    if (directory && *to_cluster != *from_cluster)
    {
        if (const Status moved = SetParent(source.file.first_cluster, *to_cluster); !moved)
        {
            return moved;
        }
    }
    return MarkDeleted(*from_cluster, source.first_slot, source.file.slot.index);
}

// FAT16 writes every sector through at once, so only the device may still hold data back.
Status Fat16Filesystem::Sync()
{
    return volume.Flush();
}

Status Fat16Filesystem::MakeReadOnly()
{
    if (const Status flushed = volume.Flush(); !flushed)
    {
        return flushed;
    }
    read_only = true;
    return {};
}

std::string_view Fat16FilesystemType::Name() const
{
    return "fat16";
}

bool Fat16FilesystemType::Probe(BlockDevice& volume)
{
    return ReadVolumeGeometry(volume).has_value();
}

Result<Filesystem*> Fat16FilesystemType::Mount(BlockDevice& volume, MountOptions options)
{
    const Result<Geometry> geometry = ReadVolumeGeometry(volume);
    if (!geometry)
    {
        return Failure(geometry);
    }
    void* memory = Host::Allocate(sizeof(Fat16Filesystem));
    if (memory == nullptr)
    {
        return std::unexpected(Error::OutOfMemory);
    }
    Filesystem* filesystem = new (memory) Fat16Filesystem(volume, *geometry, options.read_only);
    return filesystem;
}

Status Fat16FilesystemType::Unmount(Filesystem& filesystem)
{
    auto& fat = static_cast<Fat16Filesystem&>(filesystem);
    if (const Status synced = fat.Sync(); !synced)
    {
        return synced;
    }
    fat.~Fat16Filesystem();
    Host::Free(&fat);
    return {};
}

void Fat16FilesystemType::Discard(Filesystem& filesystem)
{
    auto& fat = static_cast<Fat16Filesystem&>(filesystem);
    fat.~Fat16Filesystem();
    Host::Free(&fat);
}

constinit Fat16FilesystemType fat16_type;

} // namespace

FilesystemType& Fat16Type()
{
    return fat16_type;
}

Result<bool> StatusBitsClean(BlockDevice& volume)
{
    const Result<Geometry> geometry = ReadVolumeGeometry(volume);
    if (!geometry)
    {
        return Failure(geometry);
    }
    Sector sector;
    if (const Status read = volume.Read(geometry->reserved_sectors, sector); !read)
    {
        return Failure(read);
    }
    const std::uint16_t status = Load16(sector.data() + 2);
    constexpr std::uint16_t Required = Fat16CleanShutdownBit | Fat16NoErrorBit;
    return (status & Required) == Required;
}

} // namespace FeatherDisk::Fat
