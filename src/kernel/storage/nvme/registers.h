#ifndef FEATHEROS_STORAGE_NVME_REGISTERS_H
#define FEATHEROS_STORAGE_NVME_REGISTERS_H

#include <stdint.h>

namespace FeatherOS::Storage::Nvme
{

namespace Reg
{
constexpr uint32_t Cap = 0x00;
constexpr uint32_t Vs = 0x08;
constexpr uint32_t IntMs = 0x0C;
constexpr uint32_t IntMc = 0x10;
constexpr uint32_t Cc = 0x14;
constexpr uint32_t Csts = 0x1C;
constexpr uint32_t Nssr = 0x20;
constexpr uint32_t Aqa = 0x24;
constexpr uint32_t Asq = 0x28;
constexpr uint32_t Acq = 0x30;
constexpr uint32_t DoorbellBase = 0x1000;
} // namespace Reg

namespace CcBit
{
constexpr uint32_t Enable = 1U << 0;
constexpr uint32_t ShutdownShift = 14;
constexpr uint32_t ShutdownMask = 3U << ShutdownShift;
constexpr uint32_t ShutdownNormal = 1U << ShutdownShift;
constexpr uint32_t IoSqEntrySizeShift = 16;
constexpr uint32_t IoCqEntrySizeShift = 20;
} // namespace CcBit

namespace CstsBit
{
constexpr uint32_t Ready = 1U << 0;
constexpr uint32_t FatalStatus = 1U << 1;
constexpr uint32_t ShutdownShift = 2;
constexpr uint32_t ShutdownMask = 3U << ShutdownShift;
constexpr uint32_t ShutdownComplete = 2U << ShutdownShift;
} // namespace CstsBit

constexpr uint32_t SqEntryLog2 = 6;
constexpr uint32_t CqEntryLog2 = 4;

constexpr uint32_t ControllerCc(uint32_t enable)
{
    return enable | (SqEntryLog2 << CcBit::IoSqEntrySizeShift) |
           (CqEntryLog2 << CcBit::IoCqEntrySizeShift);
}

constexpr uint32_t AdminQueueAttributes(uint32_t sq_entries, uint32_t cq_entries)
{
    return ((cq_entries - 1) << 16) | (sq_entries - 1);
}

struct Capabilities
{
    uint32_t max_queue_entries;
    bool contiguous_queues_required;
    uint32_t timeout_ms;
    uint32_t doorbell_stride;
    bool nvm_command_set;
    uint32_t min_page_bytes;
    uint32_t max_page_bytes;
};

constexpr Capabilities DecodeCap(uint64_t cap)
{
    Capabilities c{};
    c.max_queue_entries = static_cast<uint32_t>(cap & 0xFFFF) + 1;
    c.contiguous_queues_required = ((cap >> 16) & 1) != 0;
    c.timeout_ms = static_cast<uint32_t>((cap >> 24) & 0xFF) * 500;
    c.doorbell_stride = 4U << ((cap >> 32) & 0xF);
    c.nvm_command_set = ((cap >> 37) & 1) != 0;
    c.min_page_bytes = 4096U << ((cap >> 48) & 0xF);
    c.max_page_bytes = 4096U << ((cap >> 52) & 0xF);
    return c;
}

constexpr bool Supports4KiBPages(const Capabilities& c)
{
    return c.min_page_bytes == 4096;
}

constexpr uint32_t SqTailDoorbell(uint32_t qid, uint32_t stride)
{
    return Reg::DoorbellBase + (2 * qid) * stride;
}

constexpr uint32_t CqHeadDoorbell(uint32_t qid, uint32_t stride)
{
    return Reg::DoorbellBase + (2 * qid + 1) * stride;
}

struct Version
{
    uint16_t major;
    uint8_t minor;
    uint8_t tertiary;
};

constexpr Version DecodeVersion(uint32_t vs)
{
    return {static_cast<uint16_t>(vs >> 16), static_cast<uint8_t>(vs >> 8),
            static_cast<uint8_t>(vs)};
}

constexpr bool HasActiveNamespaceList(const Version& v)
{
    return v.major > 1 || (v.major == 1 && v.minor >= 1);
}

} // namespace FeatherOS::Storage::Nvme

#endif
