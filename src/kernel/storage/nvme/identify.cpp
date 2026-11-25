#include "identify.h"

#include "command.h"

#include <stddef.h>

namespace FeatherOS::Storage::Nvme
{
namespace
{

constexpr uint32_t ListEntries = IdentifyBytes / sizeof(uint32_t);
constexpr uint32_t LbaFormatOffset = 128;
constexpr uint32_t MaxLbaFormats = 64;
constexpr uint32_t MaxMdtsShift = 20;
constexpr uint32_t BroadcastNamespace = 0xFFFFFFFF;

using Bytes = std::span<const uint8_t, IdentifyBytes>;

uint64_t Get(Bytes bytes, uint32_t offset, uint32_t count)
{
    uint64_t value = 0;
    for (uint32_t i = 0; i < count; i++)
    {
        value |= static_cast<uint64_t>(bytes[offset + i]) << (8 * i);
    }
    return value;
}

template <size_t N> void CopyText(Bytes bytes, uint32_t offset, uint32_t count, char (&out)[N])
{
    static_assert(N >= 1);
    uint32_t length = count < N - 1 ? count : static_cast<uint32_t>(N - 1);
    for (uint32_t i = 0; i < length; i++)
    {
        const uint8_t c = bytes[offset + i];
        out[i] = (c >= 0x20 && c <= 0x7E) ? static_cast<char>(c) : '?';
    }
    while (length > 0 && out[length - 1] == ' ')
    {
        length--;
    }
    out[length] = '\0';
    for (size_t i = length + 1; i < N; i++)
    {
        out[i] = '\0';
    }
}

} // namespace

ControllerData ParseController(Bytes bytes)
{
    ControllerData data{};
    data.vendor_id = static_cast<uint16_t>(Get(bytes, 0, 2));
    data.subsystem_vendor_id = static_cast<uint16_t>(Get(bytes, 2, 2));
    CopyText(bytes, 4, 20, data.serial);
    CopyText(bytes, 24, 40, data.model);
    CopyText(bytes, 64, 8, data.firmware);
    data.version = static_cast<uint32_t>(Get(bytes, 80, 4));
    data.rtd3_entry_latency_us = static_cast<uint32_t>(Get(bytes, 88, 4));

    const uint8_t mdts = bytes[77];
    data.max_transfer_bytes = 0;
    if (mdts != 0)
    {
        data.max_transfer_bytes = mdts >= MaxMdtsShift ? (uint64_t{PageBytes} << MaxMdtsShift)
                                                       : (uint64_t{PageBytes} << mdts);
    }

    const uint8_t sqes = bytes[512];
    const uint8_t cqes = bytes[513];
    data.sq_entry_size_min_log2 = sqes & 0xF;
    data.sq_entry_size_max_log2 = sqes >> 4;
    data.cq_entry_size_min_log2 = cqes & 0xF;
    data.cq_entry_size_max_log2 = cqes >> 4;
    data.namespace_count = static_cast<uint32_t>(Get(bytes, 516, 4));
    data.volatile_write_cache = (bytes[525] & 1) != 0;
    return data;
}

bool SupportsEntrySizes(const ControllerData& data)
{
    return data.sq_entry_size_min_log2 <= 6 && data.sq_entry_size_max_log2 >= 6 &&
           data.cq_entry_size_min_log2 <= 4 && data.cq_entry_size_max_log2 >= 4;
}

uint32_t BounceBufferBytes(const ControllerData& data)
{
    if (data.max_transfer_bytes == 0 || data.max_transfer_bytes > BounceBytes)
    {
        return BounceBytes;
    }
    return static_cast<uint32_t>(data.max_transfer_bytes);
}

NamespaceData ParseNamespace(Bytes bytes)
{
    NamespaceData data{};
    data.sector_count = Get(bytes, 0, 8);
    data.used_sectors = Get(bytes, 16, 8);
    data.write_protected = (bytes[99] & 1) != 0;
    if (data.sector_count == 0)
    {
        data.refusal = Refusal::Inactive;
        return data;
    }

    const uint32_t format_count = static_cast<uint32_t>(bytes[25]) + 1;
    const uint8_t flbas = bytes[26];
    uint32_t index = flbas & 0xF;
    if (format_count > 16)
    {
        index |= static_cast<uint32_t>((flbas >> 5) & 3) << 4;
    }
    if (index >= format_count || index >= MaxLbaFormats)
    {
        data.refusal = Refusal::BadFormat;
        return data;
    }
    data.format_index = static_cast<uint8_t>(index);

    const uint32_t format = static_cast<uint32_t>(Get(bytes, LbaFormatOffset + 4 * index, 4));
    data.metadata_bytes = static_cast<uint16_t>(format & 0xFFFF);
    const uint32_t lba_shift = (format >> 16) & 0xFF;
    if (data.metadata_bytes != 0)
    {
        data.refusal = Refusal::Metadata;
    }
    else if (lba_shift != 9 && lba_shift != 12)
    {
        data.refusal = Refusal::SectorSize;
    }
    else if ((bytes[29] & 7) != 0)
    {
        data.refusal = Refusal::ProtectionInfo;
    }
    if (lba_shift == 9 || lba_shift == 12)
    {
        data.sector_size = 1U << lba_shift;
    }
    return data;
}

const char* RefusalText(Refusal refusal)
{
    switch (refusal)
    {
    case Refusal::None:
        return "accepted";
    case Refusal::Inactive:
        return "not active";
    case Refusal::BadFormat:
        return "LBA format out of range";
    case Refusal::Metadata:
        return "LBA format with metadata not supported";
    case Refusal::SectorSize:
        return "sector size other than 512 or 4096 not supported";
    case Refusal::ProtectionInfo:
        return "protection information not supported";
    }
    return "refused";
}

uint32_t ParseActiveList(Bytes bytes, std::span<uint32_t> out)
{
    uint32_t count = 0;
    uint32_t previous = 0;
    for (uint32_t i = 0; i < ListEntries && count < out.size(); i++)
    {
        const uint32_t nsid = static_cast<uint32_t>(Get(bytes, 4 * i, 4));
        if (nsid == 0 || nsid == BroadcastNamespace || nsid <= previous)
        {
            break;
        }
        out[count++] = nsid;
        previous = nsid;
    }
    return count;
}

} // namespace FeatherOS::Storage::Nvme
