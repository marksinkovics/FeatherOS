#include "ring.h"

#include <stddef.h>

namespace FeatherOS::Usb::Xhci
{

namespace
{

#ifdef FEATHEROS_HOST_TEST
Testing::Observer observer = nullptr;
#endif

volatile uint32_t* Slot(const Memory::Dma::Buffer& buffer, uint32_t index)
{
    return reinterpret_cast<volatile uint32_t*>(buffer.address) + static_cast<size_t>(index) * 4;
}

void Store(volatile uint32_t* address, uint32_t value)
{
    *address = value;
#ifdef FEATHEROS_HOST_TEST
    if (observer != nullptr)
    {
        observer(address, value);
    }
#endif
}

void ProducerBarrier()
{
    Memory::Dma::ProducerBarrier();
#ifdef FEATHEROS_HOST_TEST
    if (observer != nullptr)
    {
        observer(nullptr, 0);
    }
#endif
}

bool UsableSegment(const Memory::Dma::Buffer& buffer)
{
    return buffer.address != nullptr && buffer.size >= RingBytes && (buffer.physical & 63) == 0 &&
           (reinterpret_cast<uintptr_t>(buffer.address) & 15) == 0;
}

void Clear(const Memory::Dma::Buffer& buffer)
{
    for (uint32_t index = 0; index < RingTrbs * 4; ++index)
    {
        reinterpret_cast<volatile uint32_t*>(buffer.address)[index] = 0;
    }
}

} // namespace

#ifdef FEATHEROS_HOST_TEST
void Testing::SetObserver(Observer value)
{
    observer = value;
}
#endif

bool ProducerRing::Initialize(const Memory::Dma::Buffer& buffer, uint32_t padding_type)
{
    if (!UsableSegment(buffer))
    {
        return false;
    }
    Clear(buffer);
    buffer_ = buffer;
    enqueue_ = 0;
    dequeue_ = 0;
    padding_type_ = padding_type;
    cycle_ = true;
    return true;
}

uint32_t ProducerRing::Distance() const
{
    return (enqueue_ + RingTrbs - dequeue_) % RingTrbs;
}

uint32_t ProducerRing::Free() const
{
    return buffer_.address == nullptr ? 0 : RingTrbs - 1 - Distance();
}

uint64_t ProducerRing::Physical() const
{
    return buffer_.physical;
}

bool ProducerRing::CycleState() const
{
    return cycle_;
}

void ProducerRing::WriteTrb(uint32_t index, const Trb& trb)
{
    volatile uint32_t* slot = Slot(buffer_, index);
    Store(slot, trb.parameter_low);
    Store(slot + 1, trb.parameter_high);
    Store(slot + 2, trb.status);
    ProducerBarrier();
    Store(slot + 3, (trb.control & ~TrbControl::Cycle) | (cycle_ ? TrbControl::Cycle : 0));
}

void ProducerRing::WriteLink()
{
    WriteTrb(LinkIndex, MakeTrb(TrbType::Link, TrbControl::ToggleCycle, buffer_.physical, 0));
    cycle_ = !cycle_;
    enqueue_ = 0;
}

bool ProducerRing::Enqueue(std::span<const Trb> td, uint64_t* first_physical)
{
    const uint32_t count = static_cast<uint32_t>(td.size());
    if (buffer_.address == nullptr || count == 0 || count > LinkIndex)
    {
        return false;
    }
    const uint32_t before_link = LinkIndex - enqueue_;
    const bool wraps = count > before_link;
    const uint32_t cost = wraps ? before_link + 1 + count : count;
    if (cost > Free())
    {
        return false;
    }
    if (wraps)
    {
        while (enqueue_ < LinkIndex)
        {
            WriteTrb(enqueue_, MakeTrb(padding_type_, 0, 0, 0));
            ++enqueue_;
        }
        WriteLink();
    }
    if (first_physical != nullptr)
    {
        *first_physical = buffer_.physical + static_cast<uint64_t>(enqueue_) * TrbSize;
    }
    for (const Trb& trb : td)
    {
        WriteTrb(enqueue_, trb);
        ++enqueue_;
    }
    return true;
}

bool ProducerRing::InFlight(uint64_t trb_physical) const
{
    if (buffer_.address == nullptr || trb_physical < buffer_.physical ||
        trb_physical - buffer_.physical >= RingBytes || (trb_physical & (TrbSize - 1)) != 0)
    {
        return false;
    }
    const auto index = static_cast<uint32_t>((trb_physical - buffer_.physical) / TrbSize);
    return index != LinkIndex && (index + RingTrbs - dequeue_) % RingTrbs < Distance();
}

bool ProducerRing::Retire(uint64_t trb_physical)
{
    if (!InFlight(trb_physical))
    {
        return false;
    }
    dequeue_ = (static_cast<uint32_t>((trb_physical - buffer_.physical) / TrbSize) + 1) % RingTrbs;
    return true;
}

bool EventRing::Initialize(const Memory::Dma::Buffer& buffer)
{
    if (!UsableSegment(buffer))
    {
        return false;
    }
    Clear(buffer);
    buffer_ = buffer;
    dequeue_ = 0;
    cycle_ = true;
    return true;
}

bool EventRing::Next(Trb* trb)
{
    if (buffer_.address == nullptr || trb == nullptr)
    {
        return false;
    }
    volatile uint32_t* slot = Slot(buffer_, dequeue_);
    const uint32_t control = slot[3];
    if (((control & TrbControl::Cycle) != 0) != cycle_)
    {
        return false;
    }
    Memory::Dma::ConsumerBarrier();
    trb->parameter_low = slot[0];
    trb->parameter_high = slot[1];
    trb->status = slot[2];
    trb->control = control;
    if (++dequeue_ == RingTrbs)
    {
        dequeue_ = 0;
        cycle_ = !cycle_;
    }
    return true;
}

uint64_t EventRing::DequeuePointer() const
{
    return buffer_.physical + static_cast<uint64_t>(dequeue_) * TrbSize + EventHandlerBusy;
}

uint64_t EventRing::Physical() const
{
    return buffer_.physical;
}

bool EventRing::ConsumerCycle() const
{
    return cycle_;
}

} // namespace FeatherOS::Usb::Xhci
