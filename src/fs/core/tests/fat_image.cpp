// SPDX-License-Identifier: MIT OR Apache-2.0
#include "tests/fat_image.h"

#include "common/little_endian.h"

#include <algorithm>
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

} // namespace FeatherDisk::Tests
