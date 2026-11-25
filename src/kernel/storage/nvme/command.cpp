#include "command.h"

namespace FeatherOS::Storage::Nvme
{
namespace
{

constexpr uint32_t PageMask = PageBytes - 1;

constexpr uint8_t StatusTypeGeneric = 0;
constexpr uint8_t StatusSuccess = 0x00;
constexpr uint8_t StatusLbaOutOfRange = 0x80;
constexpr uint8_t StatusWriteProtected = 0x20;

Sqe NewSqe(uint8_t opcode, uint16_t cid, uint32_t nsid)
{
    Sqe sqe{};
    sqe.cdw0 = opcode | (static_cast<uint32_t>(cid) << 16);
    sqe.nsid = nsid;
    return sqe;
}

} // namespace

uint16_t CompletionCid(const Cqe& cqe)
{
    return cqe.cid;
}

bool CompletionPhase(const Cqe& cqe)
{
    return (cqe.status & 1) != 0;
}

uint16_t CompletionStatus(const Cqe& cqe)
{
    return static_cast<uint16_t>(cqe.status >> 1);
}

Sqe Identify(uint16_t cid, uint32_t nsid, uint8_t cns, uint64_t prp1)
{
    Sqe sqe = NewSqe(AdminOpcode::Identify, cid, nsid);
    sqe.prp1 = prp1;
    sqe.cdw10 = cns;
    return sqe;
}

Sqe CreateIoCq(uint16_t cid, uint16_t qid, uint16_t entries, uint64_t prp1, uint16_t vector,
               bool interrupts)
{
    Sqe sqe = NewSqe(AdminOpcode::CreateIoCq, cid, 0);
    sqe.prp1 = prp1;
    sqe.cdw10 = (static_cast<uint32_t>(entries - 1) << 16) | qid;
    sqe.cdw11 = (static_cast<uint32_t>(vector) << 16) | (interrupts ? 2U : 0U) | 1U;
    return sqe;
}

Sqe CreateIoSq(uint16_t cid, uint16_t qid, uint16_t entries, uint64_t prp1, uint16_t cqid)
{
    Sqe sqe = NewSqe(AdminOpcode::CreateIoSq, cid, 0);
    sqe.prp1 = prp1;
    sqe.cdw10 = (static_cast<uint32_t>(entries - 1) << 16) | qid;
    sqe.cdw11 = (static_cast<uint32_t>(cqid) << 16) | 1U;
    return sqe;
}

Sqe SetQueueCount(uint16_t cid, uint16_t submission_zero_based, uint16_t completion_zero_based)
{
    Sqe sqe = NewSqe(AdminOpcode::SetFeatures, cid, 0);
    sqe.cdw10 = NumberOfQueuesFeature;
    sqe.cdw11 = (static_cast<uint32_t>(completion_zero_based) << 16) | submission_zero_based;
    return sqe;
}

std::optional<Sqe> ReadWrite(bool write, uint16_t cid, uint32_t nsid, uint64_t lba, uint32_t blocks,
                             uint64_t prp1, uint64_t prp2)
{
    if (blocks == 0 || blocks > MaxBlocksPerCommand)
    {
        return std::nullopt;
    }
    Sqe sqe = NewSqe(write ? NvmOpcode::Write : NvmOpcode::Read, cid, nsid);
    sqe.prp1 = prp1;
    sqe.prp2 = prp2;
    sqe.cdw10 = static_cast<uint32_t>(lba);
    sqe.cdw11 = static_cast<uint32_t>(lba >> 32);
    sqe.cdw12 = blocks - 1;
    return sqe;
}

Sqe Flush(uint16_t cid, uint32_t nsid)
{
    return NewSqe(NvmOpcode::Flush, cid, nsid);
}

Sqe Abort(uint16_t cid, uint16_t target_cid, uint16_t sqid)
{
    Sqe sqe = NewSqe(AdminOpcode::Abort, cid, 0);
    sqe.cdw10 = (static_cast<uint32_t>(target_cid) << 16) | sqid;
    return sqe;
}

bool BuildPrps(std::span<const uint64_t> pages, uint32_t offset, uint32_t length,
               uint64_t list_physical, std::span<uint64_t> list, uint64_t* prp1, uint64_t* prp2)
{
    if (length == 0 || offset >= PageBytes || (offset & 3) != 0)
    {
        return false;
    }
    const uint64_t needed = (static_cast<uint64_t>(offset) + length + PageMask) / PageBytes;
    if (needed > pages.size())
    {
        return false;
    }
    for (uint64_t i = 0; i < needed; i++)
    {
        if ((pages[i] & PageMask) != 0)
        {
            return false;
        }
    }

    uint64_t second = 0;
    if (needed == 2)
    {
        second = pages[1];
    }
    else if (needed > 2)
    {
        const uint64_t entries = needed - 1;
        if (entries > PrpEntriesPerPage || entries > list.size() || (list_physical & PageMask) != 0)
        {
            return false;
        }
        for (uint64_t i = 0; i < entries; i++)
        {
            list[i] = pages[i + 1];
        }
        second = list_physical;
    }
    *prp1 = pages[0] + offset;
    *prp2 = second;
    return true;
}

FeatherDisk::Status MapStatus(uint16_t status_field)
{
    const uint8_t code = static_cast<uint8_t>(status_field & 0xFF);
    const uint8_t type = static_cast<uint8_t>((status_field >> 8) & 7);
    if (type == StatusTypeGeneric)
    {
        if (code == StatusSuccess)
        {
            return {};
        }
        if (code == StatusLbaOutOfRange)
        {
            return std::unexpected(FeatherDisk::Error::InvalidArgument);
        }
        if (code == StatusWriteProtected)
        {
            return std::unexpected(FeatherDisk::Error::ReadOnly);
        }
    }
    return std::unexpected(FeatherDisk::Error::Io);
}

} // namespace FeatherOS::Storage::Nvme
