// SPDX-License-Identifier: MIT OR Apache-2.0
#include "fs/partition.h"

#include "common/host_buffer.h"
#include "common/little_endian.h"

#include <bit>

namespace FeatherDisk::Partition
{
namespace
{

constexpr std::uint32_t MbrSize = 512;
constexpr std::uint32_t MbrPartitionTable = 446;
constexpr std::uint32_t MbrEntrySize = 16;
constexpr std::uint32_t MbrEntryCount = 4;
constexpr std::uint8_t ProtectiveMbrType = 0xEE;
constexpr std::uint8_t Fat16Type = 0x06;
constexpr std::uint8_t Fat16LbaType = 0x0E;
constexpr std::uint8_t LinuxType = 0x83;
constexpr std::uint64_t GptHeaderSector = 1;
constexpr std::uint32_t GptHeaderMinimumSize = 92;
constexpr std::uint32_t GptMinimumEntrySize = 128;
// The UEFI specification requires room for at least 128 entries; more is unusual.
constexpr std::uint32_t GptMaximumEntryCount = 1024;

std::uint32_t Load32(const std::uint8_t* bytes)
{
    return LoadLittleEndian<std::uint32_t>(bytes);
}

std::uint64_t Load64(const std::uint8_t* bytes)
{
    return LoadLittleEndian<std::uint64_t>(bytes);
}

// CRC-32 as used by GPT (and zlib): reflected polynomial 0xEDB88320, inverted in and out. Callers
// start with 0 and feed the bytes in order.
std::uint32_t UpdateCrc32(std::uint32_t crc, const std::uint8_t* bytes, std::size_t length)
{
    crc = ~crc;
    for (std::size_t index = 0; index < length; ++index)
    {
        crc ^= bytes[index];
        for (std::uint32_t bit = 0; bit < 8; ++bit)
        {
            crc = (crc >> 1) ^ (0xEDB88320U & (0U - (crc & 1U)));
        }
    }
    return ~crc;
}

bool ValidRange(std::uint64_t first, std::uint64_t last, std::uint64_t disk_sectors)
{
    return first != 0 && first <= last && last < disk_sectors;
}

// sector is one sector's worth of scratch space.
std::size_t FindGpt(BlockDevice& device, std::span<std::uint8_t> sector, std::span<Range> ranges)
{
    const std::uint64_t disk_sectors = device.SectorCount();
    const std::size_t sector_size = sector.size();
    if (!device.Read(GptHeaderSector, sector))
    {
        return 0;
    }
    const std::uint8_t* header = sector.data();
    constexpr char Signature[] = "EFI PART";
    for (std::size_t index = 0; index < sizeof(Signature) - 1; ++index)
    {
        if (header[index] != static_cast<std::uint8_t>(Signature[index]))
        {
            return 0;
        }
    }
    const std::uint32_t header_size = Load32(header + 12);
    const std::uint32_t header_crc = Load32(header + 16);
    if (header_size < GptHeaderMinimumSize || header_size > sector_size ||
        Load64(header + 24) != GptHeaderSector)
    {
        return 0;
    }
    // The header's CRC is computed with its own field zeroed.
    const std::uint8_t zero_crc[4] = {0, 0, 0, 0};
    std::uint32_t crc = UpdateCrc32(0, header, 16);
    crc = UpdateCrc32(crc, zero_crc, sizeof(zero_crc));
    crc = UpdateCrc32(crc, header + 20, header_size - 20);
    if (crc != header_crc)
    {
        return 0;
    }

    const std::uint64_t entries_sector = Load64(header + 72);
    const std::uint32_t entry_count = Load32(header + 80);
    const std::uint32_t entry_size = Load32(header + 84);
    const std::uint32_t entries_crc = Load32(header + 88);
    if (entry_size < GptMinimumEntrySize || entry_size > sector_size ||
        sector_size % entry_size != 0 || entry_count > GptMaximumEntryCount)
    {
        return 0;
    }
    const std::uint32_t entries_per_sector = static_cast<std::uint32_t>(sector_size / entry_size);
    const std::uint64_t entry_sectors = (entry_count + entries_per_sector - 1) / entries_per_sector;
    if (!ValidRange(entries_sector, entries_sector + entry_sectors - 1, disk_sectors))
    {
        return 0;
    }

    // Partitions are recorded while the CRC is checked, but count only if the whole array is
    // intact.
    std::size_t found = 0;
    crc = 0;
    for (std::uint32_t entry = 0; entry < entry_count; ++entry)
    {
        if (entry % entries_per_sector == 0 &&
            !device.Read(entries_sector + entry / entries_per_sector, sector))
        {
            return 0;
        }
        const std::uint8_t* raw =
            sector.data() + static_cast<std::size_t>(entry % entries_per_sector) * entry_size;
        crc = UpdateCrc32(crc, raw, entry_size);
        bool used = false;
        for (std::uint32_t index = 0; index < 16; ++index)
        {
            used = used || raw[index] != 0;
        }
        const std::uint64_t first = Load64(raw + 32);
        const std::uint64_t last = Load64(raw + 40);
        if (used && ValidRange(first, last, disk_sectors) && found < ranges.size())
        {
            ranges[found++] = {first, last - first + 1};
        }
    }
    return crc == entries_crc ? found : 0;
}

// sector receives sector 0.
Scheme ReadMbr(BlockDevice& device, std::span<std::uint8_t> sector)
{
    if (!device.Read(0, sector) || sector[510] != 0x55 || sector[511] != 0xAA)
    {
        return Scheme::None;
    }
    for (std::uint32_t index = 0; index < MbrEntryCount; ++index)
    {
        const std::uint8_t* entry =
            sector.data() + MbrPartitionTable + static_cast<std::size_t>(index) * MbrEntrySize;
        if (entry[4] == ProtectiveMbrType)
        {
            return Scheme::Gpt;
        }
    }
    return Scheme::Mbr;
}

bool ValidSectorSize(std::uint32_t sector_size)
{
    return sector_size >= MbrSize && std::has_single_bit(sector_size);
}

} // namespace

Scheme Identify(BlockDevice& device)
{
    if (!ValidSectorSize(device.SectorSize()))
    {
        return Scheme::None;
    }
    HostBuffer buffer(device.SectorSize());
    return buffer.Empty() ? Scheme::None : ReadMbr(device, buffer.Bytes());
}

std::size_t Find(BlockDevice& device, std::span<Range> ranges)
{
    if (!ValidSectorSize(device.SectorSize()))
    {
        return 0;
    }
    HostBuffer buffer(device.SectorSize());
    const std::span<std::uint8_t> sector = buffer.Bytes();
    if (buffer.Empty())
    {
        return 0;
    }
    const Scheme scheme = ReadMbr(device, sector);
    if (scheme == Scheme::None)
    {
        return 0;
    }
    if (scheme == Scheme::Gpt)
    {
        return FindGpt(device, sector, ranges);
    }
    const std::uint8_t* mbr = sector.data();
    const std::uint64_t disk_sectors = device.SectorCount();
    std::size_t found = 0;
    for (std::uint32_t index = 0; index < MbrEntryCount && found < ranges.size(); ++index)
    {
        const std::uint8_t* entry =
            mbr + MbrPartitionTable + static_cast<std::size_t>(index) * MbrEntrySize;
        const std::uint64_t first = Load32(entry + 8);
        const std::uint64_t count = Load32(entry + 12);
        const std::uint8_t type = entry[4];
        if ((type == Fat16Type || type == Fat16LbaType || type == LinuxType) && count != 0 &&
            ValidRange(first, first + count - 1, disk_sectors))
        {
            ranges[found++] = {first, count};
        }
    }
    return found;
}

} // namespace FeatherDisk::Partition
