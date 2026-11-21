#ifndef FEATHEROS_USB_XHCI_RING_H
#define FEATHEROS_USB_XHCI_RING_H

#include "kernel/memory/dma.h"
#include "kernel/usb/xhci/trb.h"

#include <span>
#include <stdint.h>

namespace FeatherOS::Usb::Xhci
{

constexpr uint32_t RingTrbs = 256;
constexpr uint32_t RingBytes = RingTrbs * TrbSize;
constexpr uint32_t LinkIndex = RingTrbs - 1;
constexpr uint64_t EventHandlerBusy = 1U << 3;

// One segment closed by a Link TRB in its last slot. The caller serializes every call (ring_lock)
// and keeps the buffer alive while the controller can read it.
class ProducerRing
{
public:
    bool Initialize(const Memory::Dma::Buffer& buffer, uint32_t padding_type = TrbType::NoOp);
    bool Enqueue(std::span<const Trb> td, uint64_t* first_physical);
    bool Retire(uint64_t trb_physical);
    bool InFlight(uint64_t trb_physical) const;
    uint32_t Free() const;
    uint64_t Physical() const;
    bool CycleState() const;

private:
    uint32_t Distance() const;
    void WriteTrb(uint32_t index, const Trb& trb);
    void WriteLink();

    Memory::Dma::Buffer buffer_{};
    uint32_t enqueue_ = 0;
    uint32_t dequeue_ = 0;
    uint32_t padding_type_ = TrbType::NoOp;
    bool cycle_ = true;
};

class EventRing
{
public:
    bool Initialize(const Memory::Dma::Buffer& buffer);
    bool Next(Trb* trb);
    // The value for ERDP: the next TRB to read, with EHB set to clear the busy flag.
    uint64_t DequeuePointer() const;
    uint64_t Physical() const;
    bool ConsumerCycle() const;

private:
    Memory::Dma::Buffer buffer_{};
    uint32_t dequeue_ = 0;
    bool cycle_ = true;
};

#ifdef FEATHEROS_HOST_TEST
namespace Testing
{
// A barrier is reported with a null address.
using Observer = void (*)(const volatile uint32_t* address, uint32_t value);
void SetObserver(Observer observer);
} // namespace Testing
#endif

} // namespace FeatherOS::Usb::Xhci

#endif
