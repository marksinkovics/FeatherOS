// SPDX-License-Identifier: MIT OR Apache-2.0
#include "tests/fat_image.h"

#include "common/little_endian.h"
#include "fat/names.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <span>

namespace FeatherDisk::Tests
{

Fat16Image::Fat16Image(Fat16Layout image_layout)
    : layout(image_layout), device(512, image_layout.sectors)
{
    std::uint8_t* boot = device.Sector(0).data();
    StoreLittleEndian<std::uint16_t>(boot + 11, 512);
    boot[13] = 1;
    StoreLittleEndian<std::uint16_t>(boot + 14, 1);
    boot[16] = static_cast<std::uint8_t>(layout.fat_count);
    StoreLittleEndian(boot + 17, static_cast<std::uint16_t>(layout.root_entries));
    StoreLittleEndian(boot + 19, static_cast<std::uint16_t>(layout.sectors));
    boot[21] = 0xF8;
    StoreLittleEndian(boot + 22, static_cast<std::uint16_t>(layout.fat_sectors));
    boot[510] = 0x55;
    boot[511] = 0xAA;
}

MemoryDevice& Fat16Image::Device()
{
    return device;
}

std::uint32_t Fat16Image::RootDirectorySector() const
{
    return 1 + layout.fat_count * layout.fat_sectors;
}

std::uint8_t* Fat16Image::RootEntry(std::uint32_t index)
{
    return device.Sector(RootDirectorySector() + index / 16).data() + (index % 16) * 32;
}

std::uint16_t Fat16Image::FatEntry(std::uint32_t cluster, std::uint32_t copy)
{
    const std::uint8_t* fat = device.Sector(1 + copy * layout.fat_sectors + cluster / 256).data();
    return LoadLittleEndian<std::uint16_t>(fat + (cluster % 256) * 2);
}

void Fat16Image::SetFatEntry(std::uint32_t cluster, std::uint16_t value)
{
    for (std::uint32_t copy = 0; copy < layout.fat_count; ++copy)
    {
        std::uint8_t* fat = device.Sector(1 + copy * layout.fat_sectors + cluster / 256).data();
        StoreLittleEndian(fat + (cluster % 256) * 2, value);
    }
}

void Fat16Image::AddRootFile(const std::string& contents)
{
    std::uint8_t* entry = RootEntry(0);
    std::memcpy(entry, "BLOCK   TXT", 11);
    entry[11] = 0x20;
    StoreLittleEndian<std::uint16_t>(entry + 26, 2);
    StoreLittleEndian(entry + 28, static_cast<std::uint32_t>(contents.size()));

    const std::uint32_t data_start = RootDirectorySector() + (layout.root_entries * 32 + 511) / 512;
    const std::size_t first_chunk = std::min<std::size_t>(contents.size(), 512);
    std::memcpy(device.Sector(data_start).data(), contents.data(), first_chunk);
    if (contents.size() > first_chunk)
    {
        std::memcpy(device.Sector(data_start + 1).data(), contents.data() + first_chunk,
                    contents.size() - first_chunk);
    }
    SetFatEntry(2, contents.size() > first_chunk ? 3 : 0xFFF8);
    SetFatEntry(3, 0xFFF8);
}

namespace
{

struct Volume
{
    MemoryDevice& device;
    std::uint32_t reserved;
    std::uint32_t fat_count;
    std::uint32_t fat_sectors;
    std::uint32_t root_entries;
    std::uint32_t sectors_per_cluster;
    std::uint32_t data_start;
    std::uint32_t clusters;

    explicit Volume(MemoryDevice& image) : device(image)
    {
        const std::uint8_t* boot = image.Sector(0).data();
        reserved = LoadLittleEndian<std::uint16_t>(boot + 14);
        fat_count = boot[16];
        fat_sectors = LoadLittleEndian<std::uint16_t>(boot + 22);
        root_entries = LoadLittleEndian<std::uint16_t>(boot + 17);
        sectors_per_cluster = boot[13];
        const std::uint32_t total = LoadLittleEndian<std::uint16_t>(boot + 19) != 0
                                        ? LoadLittleEndian<std::uint16_t>(boot + 19)
                                        : LoadLittleEndian<std::uint32_t>(boot + 32);
        data_start = reserved + fat_count * fat_sectors + (root_entries * 32 + 511) / 512;
        clusters = (total - data_start) / sectors_per_cluster;
    }

    std::uint16_t Fat(std::uint32_t cluster) const
    {
        const std::uint8_t* fat = device.Sector(reserved + cluster / 256).data();
        return LoadLittleEndian<std::uint16_t>(fat + (cluster % 256) * 2);
    }

    const std::uint8_t* ClusterBytes(std::uint32_t cluster) const
    {
        return device.Sector(data_start + std::uint64_t{cluster - 2} * sectors_per_cluster).data();
    }

    // Follows a chain to its end, stopping at a free cluster, a bad value or a loop.
    std::vector<std::uint32_t> Chain(std::uint32_t first) const
    {
        std::vector<std::uint32_t> chain;
        for (std::uint32_t cluster = first;
             cluster >= 2 && cluster < clusters + 2 && chain.size() <= clusters;)
        {
            chain.push_back(cluster);
            const std::uint16_t next = Fat(cluster);
            if (next < 2 || next >= clusters + 2)
            {
                break;
            }
            cluster = next;
        }
        return chain;
    }
};

void ScanDirectory(const Volume& volume, std::uint32_t directory, const std::string& prefix,
                   FatScan& scan, int depth)
{
    std::vector<const std::uint8_t*> slots;
    if (directory == 0)
    {
        const std::uint8_t* root =
            volume.device.Sector(volume.reserved + volume.fat_count * volume.fat_sectors).data();
        for (std::uint32_t index = 0; index < volume.root_entries; ++index)
        {
            slots.push_back(root + index * 32);
        }
    }
    else
    {
        for (const std::uint32_t cluster : volume.Chain(directory))
        {
            for (std::uint32_t index = 0; index < volume.sectors_per_cluster * 16; ++index)
            {
                slots.push_back(volume.ClusterBytes(cluster) + index * 32);
            }
        }
    }
    Fat::LongNameReader reader;
    reader.Reset();
    for (std::uint32_t index = 0; index < slots.size(); ++index)
    {
        const std::uint8_t* entry = slots[index];
        if (entry[0] == 0x00)
        {
            break;
        }
        if (entry[0] == 0xE5 || (entry[11] != Fat::LongNameAttribute && (entry[11] & 0x08) != 0))
        {
            reader.Reset();
            continue;
        }
        if (entry[11] == Fat::LongNameAttribute)
        {
            reader.Feed(entry, index);
            continue;
        }
        Fat::Name name{};
        Fat::Name short_name{};
        std::uint32_t first_slot = index;
        Fat::ShortNameText(entry, short_name);
        if (!reader.Finish(entry, name, first_slot))
        {
            name = short_name;
        }
        reader.Reset();
        if (entry[0] == '.')
        {
            continue;
        }
        FatEntryInfo info;
        info.path = prefix + std::string(name.View());
        info.short_name = std::string(short_name.View());
        info.directory = directory;
        info.slot = index;
        info.first_cluster = LoadLittleEndian<std::uint16_t>(entry + 26);
        info.size = LoadLittleEndian<std::uint32_t>(entry + 28);
        info.is_directory = (entry[11] & 0x10) != 0;
        info.chain = info.first_cluster == 0 ? std::vector<std::uint32_t>{}
                                             : volume.Chain(info.first_cluster);
        scan.entries.push_back(info);
        if (info.is_directory && depth < 8 && !info.chain.empty())
        {
            ScanDirectory(volume, info.first_cluster, info.path + "/", scan, depth + 1);
        }
    }
}

std::string Lower(std::string text)
{
    for (char& letter : text)
    {
        letter = static_cast<char>(std::tolower(static_cast<unsigned char>(letter)));
    }
    return text;
}

} // namespace

FatScan ScanFat16(MemoryDevice& device)
{
    const Volume volume(device);
    FatScan scan;
    ScanDirectory(volume, 0, "", scan, 0);
    for (const FatEntryInfo& entry : scan.entries)
    {
        for (const std::uint32_t cluster : entry.chain)
        {
            scan.owners[cluster].push_back(entry.path);
            if (volume.Fat(cluster) == 0)
            {
                scan.freed_but_owned.push_back(cluster);
            }
        }
    }
    for (std::uint32_t cluster = 2; cluster < volume.clusters + 2; ++cluster)
    {
        if (volume.Fat(cluster) != 0 && !scan.owners.contains(cluster))
        {
            scan.lost.push_back(cluster);
        }
    }
    return scan;
}

const FatEntryInfo* FatScan::Find(const std::string& path) const
{
    for (const FatEntryInfo& entry : entries)
    {
        if (Lower(entry.path) == Lower(path))
        {
            return &entry;
        }
    }
    return nullptr;
}

std::vector<const FatEntryInfo*> FatScan::Referencing(std::uint32_t cluster) const
{
    std::vector<const FatEntryInfo*> found;
    for (const FatEntryInfo& entry : entries)
    {
        if (entry.first_cluster == cluster)
        {
            found.push_back(&entry);
        }
    }
    return found;
}

std::string FatContents(MemoryDevice& device, const FatEntryInfo& entry)
{
    const Volume volume(device);
    std::string text;
    for (const std::uint32_t cluster : entry.chain)
    {
        text.append(reinterpret_cast<const char*>(volume.ClusterBytes(cluster)),
                    volume.sectors_per_cluster * 512);
    }
    text.resize(std::min<std::size_t>(text.size(), entry.size));
    return text;
}

std::uint16_t FatValue(MemoryDevice& device, std::uint32_t cluster)
{
    return Volume(device).Fat(cluster);
}

} // namespace FeatherDisk::Tests
