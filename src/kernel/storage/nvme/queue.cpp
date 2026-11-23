#include "queue.h"

#include "registers.h"

namespace FeatherOS::Storage::Nvme
{
namespace
{

constexpr uint32_t SqeWords = sizeof(Sqe) / sizeof(uint32_t);
constexpr uint32_t CqeWords = sizeof(Cqe) / sizeof(uint32_t);

volatile uint32_t* Words(const Memory::Dma::Buffer& buffer, uint32_t index, uint32_t words)
{
    return reinterpret_cast<volatile uint32_t*>(buffer.address) +
           static_cast<size_t>(index) * words;
}

} // namespace

void QueuePair::Initialize(ControllerHardware& hardware, uint16_t qid, uint16_t entries,
                           uint32_t doorbell_stride, const Memory::Dma::Buffer& sq,
                           const Memory::Dma::Buffer& cq)
{
    hardware_ = &hardware;
    qid_ = qid;
    entries_ = entries;
    stride_ = doorbell_stride;
    sq_ = sq;
    cq_ = cq;
    Reset();
}

void QueuePair::Reset()
{
    for (uint32_t i = 0; i < entries_; i++)
    {
        volatile uint32_t* sqe = Words(sq_, i, SqeWords);
        for (uint32_t w = 0; w < SqeWords; w++)
        {
            sqe[w] = 0;
        }
        volatile uint32_t* cqe = Words(cq_, i, CqeWords);
        for (uint32_t w = 0; w < CqeWords; w++)
        {
            cqe[w] = 0;
        }
    }
    tail_ = 0;
    sq_head_ = 0;
    head_ = 0;
    phase_ = true;
}

uint16_t QueuePair::NextCid()
{
    const uint16_t cid = next_cid_;
    next_cid_ = static_cast<uint16_t>(next_cid_ + 1);
    if (next_cid_ == NoCid)
    {
        next_cid_ = 0;
    }
    return cid;
}

uint16_t QueuePair::Submit(Sqe sqe)
{
    const uint16_t next = static_cast<uint16_t>((tail_ + 1) % entries_);
    if (next == sq_head_)
    {
        return NoCid;
    }
    const uint16_t cid = NextCid();
    sqe.cdw0 = (sqe.cdw0 & 0xFFFF) | (static_cast<uint32_t>(cid) << 16);
    const auto* source = reinterpret_cast<const uint32_t*>(&sqe);
    volatile uint32_t* slot = Words(sq_, tail_, SqeWords);
    for (uint32_t w = 0; w < SqeWords; w++)
    {
        slot[w] = source[w];
    }
    tail_ = next;
    Memory::Dma::ProducerBarrier();
    hardware_->Write32(SqTailDoorbell(qid_, stride_), tail_);
    return cid;
}

bool QueuePair::Reap(uint16_t wanted_cid, Cqe* out, StaleCompletions* stale)
{
    bool found = false;
    bool consumed = false;
    for (;;)
    {
        volatile uint32_t* slot = Words(cq_, head_, CqeWords);
        const uint32_t dw3 = slot[3];
        if ((((dw3 >> 16) & 1) != 0) != phase_)
        {
            break;
        }
        Memory::Dma::ConsumerBarrier();
        Cqe cqe{};
        cqe.result = slot[0];
        cqe.reserved = slot[1];
        const uint32_t dw2 = slot[2];
        cqe.sq_head = static_cast<uint16_t>(dw2 & 0xFFFF);
        cqe.sq_id = static_cast<uint16_t>(dw2 >> 16);
        cqe.cid = static_cast<uint16_t>(dw3 & 0xFFFF);
        cqe.status = static_cast<uint16_t>(dw3 >> 16);
        if (cqe.sq_head < entries_)
        {
            sq_head_ = cqe.sq_head;
        }
        if (!found && wanted_cid != NoCid && cqe.cid == wanted_cid)
        {
            *out = cqe;
            found = true;
        }
        else
        {
            ++stale->count;
            stale->last_cid = cqe.cid;
        }
        head_ = static_cast<uint16_t>(head_ + 1);
        if (head_ == entries_)
        {
            head_ = 0;
            phase_ = !phase_;
        }
        consumed = true;
    }
    if (consumed)
    {
        hardware_->Write32(CqHeadDoorbell(qid_, stride_), head_);
    }
    return found;
}

uint16_t QueuePair::Entries() const
{
    return entries_;
}

} // namespace FeatherOS::Storage::Nvme
