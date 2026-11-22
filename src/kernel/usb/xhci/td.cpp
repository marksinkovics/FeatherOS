#include "td.h"

#include "kernel/usb/host_controller.h"

#include <algorithm>

namespace FeatherOS::Usb::Xhci
{
namespace
{

constexpr uint64_t Window = 0x10000;
constexpr uint32_t MaxTdSizePackets = 31;

uint32_t FirstChunk(uint64_t physical, uint32_t length)
{
    return std::min<uint32_t>(length, static_cast<uint32_t>(Window - (physical & (Window - 1))));
}

uint32_t TrbCount(uint64_t physical, uint32_t length)
{
    return length == FirstChunk(physical, length) ? 1 : 2;
}

} // namespace

uint32_t TdBytesBefore(uint64_t physical, uint32_t length, uint32_t index)
{
    if (index == 0)
    {
        return 0;
    }
    return std::min(length, FirstChunk(physical, length) + (index - 1) * MaxBulkTransferBytes);
}

uint32_t TdTrbLength(uint64_t physical, uint32_t length, uint32_t index)
{
    if (index >= TrbCount(physical, length))
    {
        return 0;
    }
    return index == 0
               ? FirstChunk(physical, length)
               : std::min(length - TdBytesBefore(physical, length, index), MaxBulkTransferBytes);
}

uint32_t BuildNormalTd(uint64_t physical, uint32_t length, bool in, uint32_t max_packet,
                       std::span<Trb> out)
{
    if (length > MaxBulkTransferBytes || max_packet == 0)
    {
        return 0;
    }
    const uint32_t count = TrbCount(physical, length);
    if (out.size() < count)
    {
        return 0;
    }
    for (uint32_t index = 0; index < count; ++index)
    {
        const bool last = index + 1 == count;
        const uint32_t chunk = TdTrbLength(physical, length, index);
        const uint32_t after = length - TdBytesBefore(physical, length, index) - chunk;
        const uint32_t packets = std::min(MaxTdSizePackets, (after + max_packet - 1) / max_packet);
        const uint32_t flags = (last ? TrbControl::InterruptOnCompletion : TrbControl::Chain) |
                               (in ? TrbControl::InterruptOnShortPacket : 0);
        out[index] = MakeTrb(TrbType::Normal, flags,
                             length == 0 ? 0 : physical + TdBytesBefore(physical, length, index),
                             chunk | (packets << TdSizeShift));
    }
    return count;
}

} // namespace FeatherOS::Usb::Xhci
