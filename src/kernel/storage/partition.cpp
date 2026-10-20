#include "partition.h"

#include "kernel/storage/block_device.h"

namespace FeatherOS::Partition
{
namespace
{

constexpr uint32_t SectorSize = BlockDevice::SectorSize;
constexpr uint32_t MbrPartitionTable = 446;
constexpr uint32_t MbrEntrySize = 16;
constexpr uint8_t ProtectiveMbrType = 0xEE;
constexpr uint8_t Fat16Type = 0x06;
constexpr uint8_t Fat16LbaType = 0x0E;
constexpr uint64_t GptHeaderSector = 1;
constexpr uint32_t GptHeaderMinimumSize = 92;
constexpr uint32_t GptMinimumEntrySize = 128;
// The UEFI specification requires room for at least 128 entries; more is unusual.
constexpr uint32_t GptMaximumEntryCount = 1024;

uint32_t ReadLittleEndian32(const uint8_t* bytes)
{
    return static_cast<uint32_t>(bytes[0]) | (static_cast<uint32_t>(bytes[1]) << 8) |
           (static_cast<uint32_t>(bytes[2]) << 16) | (static_cast<uint32_t>(bytes[3]) << 24);
}

uint64_t ReadLittleEndian64(const uint8_t* bytes)
{
    return static_cast<uint64_t>(ReadLittleEndian32(bytes)) |
           (static_cast<uint64_t>(ReadLittleEndian32(bytes + 4)) << 32);
}

// CRC-32 as used by GPT (and zlib): reflected polynomial 0xEDB88320, inverted in and out. Callers
// start with 0 and feed the bytes in order.
uint32_t UpdateCrc32(uint32_t crc, const uint8_t* bytes, uint32_t length)
{
    crc = ~crc;
    for (uint32_t index = 0; index < length; ++index)
    {
        crc ^= bytes[index];
        for (uint32_t bit = 0; bit < 8; ++bit)
        {
            crc = (crc >> 1) ^ (0xEDB88320U & (0U - (crc & 1U)));
        }
    }
    return ~crc;
}

bool ValidRange(uint64_t first, uint64_t last, uint64_t disk_sectors)
{
    return first != 0 && first <= last && last < disk_sectors;
}

uint32_t FindGpt(Range (&ranges)[MaximumPartitions])
{
    const uint64_t disk_sectors = BlockDevice::SectorCount();
    uint8_t header[SectorSize];
    if (!BlockDevice::ReadSector(GptHeaderSector, header))
    {
        return 0;
    }
    constexpr char Signature[] = "EFI PART";
    for (uint32_t index = 0; index < sizeof(Signature) - 1; ++index)
    {
        if (header[index] != static_cast<uint8_t>(Signature[index]))
        {
            return 0;
        }
    }
    const uint32_t header_size = ReadLittleEndian32(header + 12);
    const uint32_t header_crc = ReadLittleEndian32(header + 16);
    if (header_size < GptHeaderMinimumSize || header_size > SectorSize ||
        ReadLittleEndian64(header + 24) != GptHeaderSector)
    {
        return 0;
    }
    // The header's CRC is computed with its own field zeroed.
    uint8_t zero_crc[4] = {0, 0, 0, 0};
    uint32_t crc = UpdateCrc32(0, header, 16);
    crc = UpdateCrc32(crc, zero_crc, sizeof(zero_crc));
    crc = UpdateCrc32(crc, header + 20, header_size - 20);
    if (crc != header_crc)
    {
        return 0;
    }

    const uint64_t entries_sector = ReadLittleEndian64(header + 72);
    const uint32_t entry_count = ReadLittleEndian32(header + 80);
    const uint32_t entry_size = ReadLittleEndian32(header + 84);
    const uint32_t entries_crc = ReadLittleEndian32(header + 88);
    if (entry_size < GptMinimumEntrySize || entry_size > SectorSize ||
        SectorSize % entry_size != 0 || entry_count > GptMaximumEntryCount)
    {
        return 0;
    }
    const uint32_t entries_per_sector = SectorSize / entry_size;
    const uint64_t entry_sectors = (entry_count + entries_per_sector - 1) / entries_per_sector;
    if (!ValidRange(entries_sector, entries_sector + entry_sectors - 1, disk_sectors))
    {
        return 0;
    }

    // Partitions are recorded while the CRC is checked, but count only if the whole array is
    // intact.
    uint32_t found = 0;
    crc = 0;
    uint8_t sector[SectorSize];
    for (uint32_t entry = 0; entry < entry_count; ++entry)
    {
        if (entry % entries_per_sector == 0 &&
            !BlockDevice::ReadSector(entries_sector + entry / entries_per_sector, sector))
        {
            return 0;
        }
        const uint8_t* raw = sector + (entry % entries_per_sector) * entry_size;
        crc = UpdateCrc32(crc, raw, entry_size);
        bool used = false;
        for (uint32_t index = 0; index < 16; ++index)
        {
            used = used || raw[index] != 0;
        }
        const uint64_t first = ReadLittleEndian64(raw + 32);
        const uint64_t last = ReadLittleEndian64(raw + 40);
        if (used && ValidRange(first, last, disk_sectors) && found < MaximumPartitions)
        {
            ranges[found++] = {first, last - first + 1};
        }
    }
    return crc == entries_crc ? found : 0;
}

} // namespace

uint32_t Find(Range (&ranges)[MaximumPartitions])
{
    uint8_t mbr[SectorSize];
    if (!BlockDevice::ReadSector(0, mbr) || mbr[510] != 0x55 || mbr[511] != 0xAA)
    {
        return 0;
    }
    const uint64_t disk_sectors = BlockDevice::SectorCount();
    uint32_t found = 0;
    for (uint32_t index = 0; index < 4; ++index)
    {
        const uint8_t* entry = mbr + MbrPartitionTable + index * MbrEntrySize;
        if (entry[4] == ProtectiveMbrType)
        {
            return FindGpt(ranges);
        }
    }
    for (uint32_t index = 0; index < 4; ++index)
    {
        const uint8_t* entry = mbr + MbrPartitionTable + index * MbrEntrySize;
        const uint64_t first = ReadLittleEndian32(entry + 8);
        const uint64_t count = ReadLittleEndian32(entry + 12);
        if ((entry[4] == Fat16Type || entry[4] == Fat16LbaType) && count != 0 &&
            ValidRange(first, first + count - 1, disk_sectors))
        {
            ranges[found++] = {first, count};
        }
    }
    return found;
}

} // namespace FeatherOS::Partition
