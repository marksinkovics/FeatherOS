#include "fake_xhci.h"

#include "kernel/usb/xhci/context.h"
#include "kernel/usb/xhci/registers.h"
#include "kernel/usb/xhci/ring.h"

#include <cstdlib>
#include <cstring>

namespace FeatherOS::Usb::Testing
{

namespace
{

namespace X = Xhci;

constexpr uint32_t TrbMask = ~static_cast<uint32_t>(X::TrbSize - 1);
constexpr uint32_t LoopLimit = 100000;
constexpr uint64_t PageSize = 4096;

uint32_t Fetch(volatile const uint32_t* address)
{
    return *address;
}

} // namespace

PhysicalMemory::~PhysicalMemory()
{
    for (const Region& region : regions_)
    {
        std::free(region.host);
    }
}

Memory::Dma::Buffer PhysicalMemory::Allocate(uint64_t bytes)
{
    const uint64_t size = (bytes + PageSize - 1) / PageSize * PageSize;
    auto* host = static_cast<uint8_t*>(std::aligned_alloc(PageSize, size));
    std::memset(host, 0, size);
    regions_.push_back({next_physical_, host, size});
    Memory::Dma::Buffer buffer{next_physical_, host, bytes};
    next_physical_ += size + PageSize;
    return buffer;
}

volatile uint8_t* PhysicalMemory::Translate(uint64_t physical, uint64_t length) const
{
    for (const Region& region : regions_)
    {
        if (physical >= region.physical && length <= region.size &&
            physical - region.physical <= region.size - length)
        {
            return region.host + (physical - region.physical);
        }
    }
    return nullptr;
}

FakeXhci::FakeXhci(PhysicalMemory& memory, uint32_t ports, bool context64)
    : memory_(memory), ports_(ports), context64_(context64), portsc_(ports, Xhci::PortSc::Power),
      enable_clears_(ports, 0)
{
}

uint32_t FakeXhci::Read32(uint32_t offset)
{
    if (offset == X::Cap::Length)
    {
        return CapabilityLength | (0x0100U << 16);
    }
    if (offset == X::Cap::HcsParams1)
    {
        return MaxSlots | (1U << 8) | (ports_ << 24);
    }
    if (offset == X::Cap::HccParams1)
    {
        return (1U << 0) | (context64_ ? 1U << 2 : 0);
    }
    if (offset == X::Cap::DoorbellOffset)
    {
        return DoorbellBase;
    }
    if (offset == X::Cap::RuntimeOffset)
    {
        return RuntimeBase;
    }
    if (offset >= CapabilityLength && offset < RuntimeBase)
    {
        const uint32_t reg = offset - CapabilityLength;
        switch (reg)
        {
        case X::Op::UsbCmd:
            return usbcmd_;
        case X::Op::UsbSts:
            return usbsts_;
        case X::Op::PageSize:
            return 1;
        case X::Op::Config:
            return config_;
        case X::Op::Dcbaap:
            return static_cast<uint32_t>(dcbaap_);
        case X::Op::Dcbaap + 4:
            return static_cast<uint32_t>(dcbaap_ >> 32);
        default:
            break;
        }
        if (reg >= X::Op::PortBase && reg < X::Op::PortBase + ports_ * X::Op::PortStride &&
            (reg - X::Op::PortBase) % X::Op::PortStride == 0)
        {
            return portsc_[(reg - X::Op::PortBase) / X::Op::PortStride];
        }
        return 0;
    }
    if (offset >= RuntimeBase && offset < DoorbellBase)
    {
        return ReadRuntimeRegister(offset - RuntimeBase);
    }
    return 0;
}

void FakeXhci::Write64(uint32_t offset, uint64_t value)
{
    Write32(offset + 4, static_cast<uint32_t>(value >> 32));
    Write32(offset, static_cast<uint32_t>(value));
}

void FakeXhci::Write32(uint32_t offset, uint32_t value)
{
    if (offset >= DoorbellBase && offset < DoorbellBase + (MaxSlots + 1) * 4)
    {
        const uint32_t index = (offset - DoorbellBase) / 4;
        if (index == 0)
        {
            ProcessCommands();
        }
        else
        {
            ProcessTransfers(index, value & 0xFFU);
        }
    }
    else if (offset >= RuntimeBase && offset < DoorbellBase)
    {
        WriteRuntimeRegister(offset - RuntimeBase, value);
    }
    else if (offset >= CapabilityLength)
    {
        WriteOperationalRegister(offset - CapabilityLength, value);
    }
}

uint32_t FakeXhci::ReadRuntimeRegister(uint32_t reg) const
{
    switch (reg)
    {
    case X::InterrupterRegister(0, X::Runtime::Iman):
        return iman_;
    case X::InterrupterRegister(0, X::Runtime::Imod):
        return imod_;
    case X::InterrupterRegister(0, X::Runtime::ErstSize):
        return erst_size_;
    case X::InterrupterRegister(0, X::Runtime::ErstBase):
        return static_cast<uint32_t>(erst_base_);
    case X::InterrupterRegister(0, X::Runtime::ErstBase) + 4:
        return static_cast<uint32_t>(erst_base_ >> 32);
    case X::InterrupterRegister(0, X::Runtime::Erdp):
        return static_cast<uint32_t>(erdp_) | (event_busy_ ? X::EventHandlerBusy : 0);
    case X::InterrupterRegister(0, X::Runtime::Erdp) + 4:
        return static_cast<uint32_t>(erdp_ >> 32);
    default:
        return 0;
    }
}

void FakeXhci::WriteRuntimeRegister(uint32_t reg, uint32_t value)
{
    switch (reg)
    {
    case X::InterrupterRegister(0, X::Runtime::Iman):
        iman_ = (iman_ & ~X::Iman::InterruptEnable & ~(value & X::Iman::InterruptPending)) |
                (value & X::Iman::InterruptEnable);
        break;
    case X::InterrupterRegister(0, X::Runtime::Imod):
        imod_ = value;
        break;
    case X::InterrupterRegister(0, X::Runtime::ErstSize):
        erst_size_ = value & 0xFFFFU;
        break;
    case X::InterrupterRegister(0, X::Runtime::ErstBase):
        erst_base_ = (erst_base_ & ~0xFFFFFFFFULL) | (value & TrbMask);
        {
            volatile uint8_t* entry = memory_.Translate(erst_base_, 16);
            if (entry == nullptr || erst_size_ == 0)
            {
                segment_size_ = 0;
                ++faults_;
                break;
            }
            const auto* words = reinterpret_cast<volatile const uint32_t*>(entry);
            segment_base_ = (static_cast<uint64_t>(Fetch(words + 1)) << 32) | Fetch(words);
            segment_size_ = Fetch(words + 2) & 0xFFFFU;
            erdp_ = segment_base_;
            event_enqueue_ = 0;
            event_cycle_ = true;
        }
        break;
    case X::InterrupterRegister(0, X::Runtime::ErstBase) + 4:
        erst_base_ = (erst_base_ & 0xFFFFFFFFULL) | (static_cast<uint64_t>(value) << 32);
        break;
    case X::InterrupterRegister(0, X::Runtime::Erdp):
        if ((value & X::EventHandlerBusy) != 0)
        {
            event_busy_ = false;
        }
        erdp_ = (erdp_ & ~0xFFFFFFFFULL) | (value & TrbMask);
        erdp_writes_.push_back(erdp_ | (value & X::EventHandlerBusy));
        break;
    case X::InterrupterRegister(0, X::Runtime::Erdp) + 4:
        erdp_ = (erdp_ & 0xFFFFFFFFULL) | (static_cast<uint64_t>(value) << 32);
        break;
    default:
        break;
    }
}

void FakeXhci::WriteOperationalRegister(uint32_t reg, uint32_t value)
{
    switch (reg)
    {
    case X::Op::UsbCmd:
        if ((value & X::UsbCmd::HostControllerReset) != 0)
        {
            Reset();
            return;
        }
        usbcmd_ = value;
        usbsts_ = (value & X::UsbCmd::RunStop) != 0 ? usbsts_ & ~X::UsbSts::HostControllerHalted
                                                    : usbsts_ | X::UsbSts::HostControllerHalted;
        return;
    case X::Op::UsbSts:
        usbsts_ &= ~(value & (X::UsbSts::HostSystemError | X::UsbSts::EventInterrupt |
                              X::UsbSts::PortChangeDetect));
        return;
    case X::Op::Config:
        config_ = value;
        return;
    case X::Op::CommandRing:
        command_dequeue_ = (command_dequeue_ & ~0xFFFFFFFFULL) | (value & 0xFFFFFFC0U);
        command_cycle_ = (value & X::CommandRingControl::RingCycleState) != 0;
        return;
    case X::Op::CommandRing + 4:
        command_dequeue_ = (command_dequeue_ & 0xFFFFFFFFULL) | (static_cast<uint64_t>(value) << 32);
        return;
    case X::Op::Dcbaap:
        dcbaap_ = (dcbaap_ & ~0xFFFFFFFFULL) | (value & 0xFFFFFFC0U);
        return;
    case X::Op::Dcbaap + 4:
        dcbaap_ = (dcbaap_ & 0xFFFFFFFFULL) | (static_cast<uint64_t>(value) << 32);
        return;
    default:
        break;
    }
    if (reg >= X::Op::PortBase && reg < X::Op::PortBase + ports_ * X::Op::PortStride &&
        (reg - X::Op::PortBase) % X::Op::PortStride == 0)
    {
        WritePortRegister((reg - X::Op::PortBase) / X::Op::PortStride + 1, value);
    }
}

void FakeXhci::Reset()
{
    usbcmd_ = 0;
    usbsts_ = X::UsbSts::HostControllerHalted;
    config_ = 0;
    dcbaap_ = 0;
    command_dequeue_ = 0;
    command_cycle_ = true;
    iman_ = 0;
    erst_size_ = 0;
    erst_base_ = 0;
    segment_base_ = 0;
    segment_size_ = 0;
    erdp_ = 0;
    event_enqueue_ = 0;
    event_cycle_ = true;
    event_busy_ = false;
    next_slot_ = 1;
    endpoints_.clear();
}

bool FakeXhci::ReadTrb(uint64_t physical, X::Trb* trb)
{
    volatile uint8_t* bytes = memory_.Translate(physical, X::TrbSize);
    if (bytes == nullptr || (physical & (X::TrbSize - 1)) != 0)
    {
        ++faults_;
        return false;
    }
    const auto* words = reinterpret_cast<volatile const uint32_t*>(bytes);
    trb->control = Fetch(words + 3);
    trb->parameter_low = Fetch(words);
    trb->parameter_high = Fetch(words + 1);
    trb->status = Fetch(words + 2);
    return true;
}

bool FakeXhci::PostEvent(const X::Trb& event)
{
    if (segment_size_ == 0)
    {
        ++dropped_events_;
        return false;
    }
    const uint64_t dequeue_index = ((erdp_ & ~static_cast<uint64_t>(X::TrbSize - 1)) - segment_base_) / X::TrbSize;
    if ((event_enqueue_ + 1) % segment_size_ == dequeue_index)
    {
        ++dropped_events_;
        return false;
    }
    volatile uint8_t* bytes =
        memory_.Translate(segment_base_ + static_cast<uint64_t>(event_enqueue_) * X::TrbSize, X::TrbSize);
    if (bytes == nullptr)
    {
        ++faults_;
        return false;
    }
    auto* words = reinterpret_cast<volatile uint32_t*>(bytes);
    words[0] = event.parameter_low;
    words[1] = event.parameter_high;
    words[2] = event.status;
    words[3] = (event.control & ~X::TrbControl::Cycle) | (event_cycle_ ? X::TrbControl::Cycle : 0);
    if (++event_enqueue_ == segment_size_)
    {
        event_enqueue_ = 0;
        event_cycle_ = !event_cycle_;
    }
    event_busy_ = true;
    iman_ |= X::Iman::InterruptPending;
    usbsts_ |= X::UsbSts::EventInterrupt;
    return true;
}

void FakeXhci::ProcessCommands()
{
    if ((usbcmd_ & X::UsbCmd::RunStop) == 0)
    {
        return;
    }
    for (uint32_t step = 0; step < LoopLimit; ++step)
    {
        X::Trb trb;
        if (!ReadTrb(command_dequeue_, &trb) || ((trb.control & X::TrbControl::Cycle) != 0) != command_cycle_)
        {
            return;
        }
        const uint64_t physical = command_dequeue_;
        const uint32_t type = X::TrbTypeOf(trb);
        if (type == X::TrbType::Link)
        {
            command_dequeue_ = X::TrbParameter(trb) & ~static_cast<uint64_t>(X::TrbSize - 1);
            if ((trb.control & X::TrbControl::ToggleCycle) != 0)
            {
                command_cycle_ = !command_cycle_;
            }
            continue;
        }
        command_dequeue_ += X::TrbSize;
        command_types_.push_back(type);
        uint32_t slot = (trb.control & X::TrbControl::SlotMask) >> X::TrbControl::SlotShift;
        uint32_t code = X::CompletionCode::Success;
        if (type == X::TrbType::EnableSlot)
        {
            if (next_slot_ > MaxSlots)
            {
                slot = 0;
                code = X::CompletionCode::NoSlotsAvailable;
            }
            else
            {
                slot = next_slot_++;
            }
        }
        PostEvent(X::MakeTrb(X::TrbType::CommandCompletionEvent, slot << X::TrbControl::SlotShift,
                             physical, code << X::CompletionShift));
    }
}

void FakeXhci::ProcessTransfers(uint32_t slot, uint32_t dci)
{
    if ((usbcmd_ & X::UsbCmd::RunStop) == 0)
    {
        return;
    }
    const auto found = endpoints_.find(slot << 5 | dci);
    if (found == endpoints_.end())
    {
        return;
    }
    Endpoint& endpoint = found->second;
    for (uint32_t step = 0; step < LoopLimit; ++step)
    {
        X::Trb trb;
        if (!ReadTrb(endpoint.dequeue, &trb) ||
            ((trb.control & X::TrbControl::Cycle) != 0) != endpoint.cycle)
        {
            return;
        }
        const uint64_t physical = endpoint.dequeue;
        const uint32_t type = X::TrbTypeOf(trb);
        if (type == X::TrbType::Link)
        {
            endpoint.dequeue = X::TrbParameter(trb) & ~static_cast<uint64_t>(X::TrbSize - 1);
            if ((trb.control & X::TrbControl::ToggleCycle) != 0)
            {
                endpoint.cycle = !endpoint.cycle;
            }
            continue;
        }
        endpoint.dequeue += X::TrbSize;
        transfer_types_.push_back(type);
        if ((trb.control & X::TrbControl::InterruptOnCompletion) != 0)
        {
            PostEvent(X::MakeTrb(X::TrbType::TransferEvent,
                                 (dci << X::TrbControl::EndpointShift) | (slot << X::TrbControl::SlotShift),
                                 physical, X::CompletionCode::Success << X::CompletionShift));
        }
    }
}

uint32_t FakeXhci::PortStatus(uint32_t port) const
{
    return portsc_[port - 1];
}

void FakeXhci::PostPortEvent(uint32_t port)
{
    usbsts_ |= X::UsbSts::PortChangeDetect;
    PostEvent(X::MakeTrb(X::TrbType::PortStatusChangeEvent, 0, static_cast<uint64_t>(port) << 24,
                         X::CompletionCode::Success << X::CompletionShift));
}

void FakeXhci::WritePortRegister(uint32_t port, uint32_t value)
{
    uint32_t& status = portsc_[port - 1];
    status &= ~(value & X::PortSc::ChangeMask);
    if ((value & X::PortSc::Enabled) != 0)
    {
        status &= ~X::PortSc::Enabled;
        ++enable_clears_[port - 1];
    }
    constexpr uint32_t Stored = X::PortSc::Power | X::PortSc::IndicatorMask |
                                X::PortSc::WakeOnConnect | X::PortSc::WakeOnDisconnect |
                                X::PortSc::WakeOnOverCurrent;
    status = (status & ~Stored) | (value & Stored);
    if ((value & X::PortSc::Reset) != 0 && (status & X::PortSc::CurrentConnect) != 0)
    {
        status |= X::PortSc::Enabled | X::PortSc::ResetChange;
        status &= ~X::PortSc::LinkStateMask;
        PostPortEvent(port);
    }
}

void FakeXhci::SetEndpointRing(uint32_t slot, uint32_t dci, uint64_t ring, bool cycle)
{
    endpoints_[slot << 5 | dci] = {ring, cycle};
}

void FakeXhci::Connect(uint32_t port, Speed speed)
{
    portsc_[port - 1] = X::PortSc::CurrentConnect | X::PortSc::ConnectChange | X::PortSc::Power |
                        (static_cast<uint32_t>(X::SpeedId(speed)) << X::PortSc::SpeedShift);
    PostPortEvent(port);
}

void FakeXhci::Disconnect(uint32_t port)
{
    uint32_t& status = portsc_[port - 1];
    status &= ~(X::PortSc::CurrentConnect | X::PortSc::Enabled | X::PortSc::SpeedMask);
    status |= X::PortSc::ConnectChange;
    PostPortEvent(port);
}

const std::vector<uint32_t>& FakeXhci::CommandTypes() const
{
    return command_types_;
}

const std::vector<uint32_t>& FakeXhci::TransferTypes() const
{
    return transfer_types_;
}

const std::vector<uint64_t>& FakeXhci::EventPointerWrites() const
{
    return erdp_writes_;
}

uint32_t FakeXhci::DroppedEvents() const
{
    return dropped_events_;
}

uint32_t FakeXhci::Faults() const
{
    return faults_;
}

uint32_t FakeXhci::PortEnableClears(uint32_t port) const
{
    return enable_clears_[port - 1];
}

} // namespace FeatherOS::Usb::Testing
