// SPDX-License-Identifier: MIT OR Apache-2.0
#include "tests/partition_image.h"

#include "common/little_endian.h"

#include <algorithm>
#include <cstring>

namespace FeatherDisk::Tests
{

void SetMbrEntry(MemoryDevice& device, std::size_t index, std::uint8_t type, std::uint32_t first,
                 std::uint32_t count)
{
    std::uint8_t* entry = device.Sector(0).data() + 446 + index * 16;
    entry[4] = type;
    StoreLittleEndian(entry + 8, first);
    StoreLittleEndian(entry + 12, count);
    device.Sector(0)[510] = 0x55;
    device.Sector(0)[511] = 0xAA;
}

std::uint32_t Crc32(const std::uint8_t* bytes, std::size_t length)
{
    std::uint32_t crc = 0xFFFFFFFFU;
    for (std::size_t index = 0; index < length; ++index)
    {
        crc ^= bytes[index];
        for (int bit = 0; bit < 8; ++bit)
        {
            crc = (crc >> 1) ^ ((crc & 1U) != 0 ? 0xEDB88320U : 0U);
        }
    }
    return ~crc;
}

std::uint8_t* GptHeader(MemoryDevice& device)
{
    return device.Sector(1).data();
}

void SealGptHeader(MemoryDevice& device)
{
    std::uint8_t* header = GptHeader(device);
    StoreLittleEndian<std::uint32_t>(header + 16, 0);
    StoreLittleEndian(header + 16, Crc32(header, LoadLittleEndian<std::uint32_t>(header + 12)));
}

void WriteGpt(MemoryDevice& device, const std::vector<GptEntry>& entries, GptLayout layout)
{
    SetMbrEntry(device, 0, 0xEE, 1, 0xFFFFFFFFU);

    std::vector<std::uint8_t> array(static_cast<std::size_t>(layout.entry_count) *
                                    layout.entry_size);
    for (const GptEntry& entry : entries)
    {
        std::uint8_t* raw = array.data() + entry.index * layout.entry_size;
        std::memcpy(raw, entry.type.data(), entry.type.size());
        StoreLittleEndian(raw + 32, entry.first);
        StoreLittleEndian(raw + 40, entry.last);
    }
    const std::uint32_t sector_size = device.SectorSize();
    for (std::size_t offset = 0; offset < array.size(); offset += sector_size)
    {
        const std::size_t length = std::min<std::size_t>(sector_size, array.size() - offset);
        std::memcpy(device.Sector(2 + offset / sector_size).data(), array.data() + offset, length);
    }

    std::uint8_t* header = GptHeader(device);
    std::memcpy(header, "EFI PART", 8);
    StoreLittleEndian<std::uint32_t>(header + 8, 0x00010000);
    StoreLittleEndian<std::uint32_t>(header + 12, 92);
    StoreLittleEndian<std::uint64_t>(header + 24, 1);
    StoreLittleEndian<std::uint64_t>(header + 32, device.SectorCount() - 1);
    StoreLittleEndian<std::uint64_t>(header + 72, 2);
    StoreLittleEndian(header + 80, layout.entry_count);
    StoreLittleEndian(header + 84, layout.entry_size);
    StoreLittleEndian(header + 88, Crc32(array.data(), array.size()));
    SealGptHeader(device);
}

} // namespace FeatherDisk::Tests
