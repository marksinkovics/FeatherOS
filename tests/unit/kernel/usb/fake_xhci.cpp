#include "fake_xhci.h"

#include "kernel/usb/xhci/context.h"
#include "kernel/usb/xhci/registers.h"
#include "kernel/usb/xhci/ring.h"

#include <algorithm>
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

PhysicalMemory::PhysicalMemory(uint64_t base) : next_physical_(base)
{
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
      enable_clears_(ports, 0), port_resets_(ports, 0)
{
    BuildProtocols();
    usbsts_ = X::UsbSts::HostControllerHalted;
}

uint32_t FakeXhci::Read32(uint32_t offset)
{
    CountNotReadyAccess(offset);
    if (offset >= ExtendedBase)
    {
        return ReadExtended(offset);
    }
    if (offset == X::Cap::Length)
    {
        return CapabilityLength | (0x0100U << 16);
    }
    if (offset == X::Cap::HcsParams1)
    {
        return MaxSlots | (1U << 8) | (ports_ << 24);
    }
    if (offset == X::Cap::HcsParams2)
    {
        return (((scratchpads_ >> 5) & 0x1FU) << 21) | ((scratchpads_ & 0x1FU) << 27);
    }
    if (offset == X::Cap::HccParams1)
    {
        return (ac64_ ? X::HccParams1::Ac64 : 0) |
               (context64_ ? X::HccParams1::ContextSize64 : 0) |
               (port_power_control_ ? X::HccParams1::PortPowerControl : 0) |
               ((ExtendedBase / 4) << X::HccParams1::ExtendedCapabilityShift);
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
            ReadStatus();
            return usbcmd_ | (reset_pending_ ? X::UsbCmd::HostControllerReset : 0);
        case X::Op::UsbSts:
            return ReadStatus();
        case X::Op::PageSize:
            return page_size_bits_;
        case X::Op::Config:
            return config_;
        case X::Op::CommandRing:
            return (command_cycle_ ? 1U : 0U) |
                   (held_ ? static_cast<uint32_t>(X::CommandRingControl::CommandRingRunning) : 0U);
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

uint32_t FakeXhci::ReadStatus()
{
    if (reset_pending_ && !never_ready_)
    {
        if (ready_countdown_ == 0)
        {
            reset_pending_ = false;
            usbsts_ &= ~X::UsbSts::ControllerNotReady;
        }
        else
        {
            --ready_countdown_;
        }
    }
    if (halt_pending_ && !never_halts_)
    {
        if (halt_countdown_ == 0)
        {
            halt_pending_ = false;
            usbsts_ |= X::UsbSts::HostControllerHalted;
        }
        else
        {
            --halt_countdown_;
        }
    }
    return usbsts_;
}

void FakeXhci::CountNotReadyAccess(uint32_t offset)
{
    if ((usbsts_ & X::UsbSts::ControllerNotReady) == 0 || offset < CapabilityLength)
    {
        return;
    }
    const uint32_t reg = offset - CapabilityLength;
    if (offset < RuntimeBase && (reg == X::Op::UsbCmd || reg == X::Op::UsbSts))
    {
        return;
    }
    ++not_ready_accesses_;
}

void FakeXhci::Write64(uint32_t offset, uint64_t value)
{
    Write32(offset + 4, static_cast<uint32_t>(value >> 32));
    Write32(offset, static_cast<uint32_t>(value));
}

void FakeXhci::Write32(uint32_t offset, uint32_t value)
{
    CountNotReadyAccess(offset);
    if (offset >= ExtendedBase)
    {
        WriteExtended(offset, value);
    }
    else if (offset >= DoorbellBase && offset < DoorbellBase + (MaxSlots + 1) * 4)
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
            resets_.push_back({(usbsts_ & X::UsbSts::HostControllerHalted) != 0, legacy_present_,
                               os_owned_, bios_owned_, smi_enables_, smi_status_});
            Reset();
            return;
        }
        {
            const bool was_running = (usbsts_ & X::UsbSts::HostControllerHalted) == 0;
            usbcmd_ = value;
            if ((value & X::UsbCmd::RunStop) != 0)
            {
                usbsts_ &= ~X::UsbSts::HostControllerHalted;
                halt_pending_ = false;
            }
            else if (was_running && !halt_pending_)
            {
                halt_pending_ = true;
                halt_countdown_ = halt_delay_;
                if (halt_delay_ == 0 && !never_halts_)
                {
                    halt_pending_ = false;
                    usbsts_ |= X::UsbSts::HostControllerHalted;
                }
            }
        }
        return;
    case X::Op::UsbSts:
        usbsts_ &= ~(value & (X::UsbSts::HostSystemError | X::UsbSts::EventInterrupt |
                              X::UsbSts::PortChangeDetect));
        return;
    case X::Op::Config:
        config_ = value;
        return;
    case X::Op::CommandRing:
        if ((value & X::CommandRingControl::CommandAbort) != 0)
        {
            AbortCommands();
        }
        else if (!held_)
        {
            command_dequeue_ = (command_dequeue_ & ~0xFFFFFFFFULL) | (value & 0xFFFFFFC0U);
            command_cycle_ = (value & X::CommandRingControl::RingCycleState) != 0;
        }
        return;
    case X::Op::CommandRing + 4:
        if (!held_)
        {
            command_dequeue_ =
                (command_dequeue_ & 0xFFFFFFFFULL) | (static_cast<uint64_t>(value) << 32);
        }
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
    halt_pending_ = false;
    reset_pending_ = reset_delay_ > 0 || never_ready_;
    ready_countdown_ = reset_delay_;
    if (reset_pending_)
    {
        usbsts_ |= X::UsbSts::ControllerNotReady;
    }
    config_ = 0;
    dcbaap_ = 0;
    command_dequeue_ = 0;
    command_cycle_ = true;
    held_ = false;
    iman_ = 0;
    imod_ = 0;
    erst_size_ = 0;
    erst_base_ = 0;
    segment_base_ = 0;
    segment_size_ = 0;
    erdp_ = 0;
    event_enqueue_ = 0;
    event_cycle_ = true;
    event_busy_ = false;
    enabled_slots_.clear();
    endpoints_.clear();
    bindings_.clear();
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
    if ((usbcmd_ & X::UsbCmd::RunStop) == 0 || held_)
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
        if (hold_type_ != 0 && hold_type_ == type)
        {
            hold_type_ = 0;
            held_ = true;
            held_trb_ = physical;
            held_command_ = trb;
            last_held_trb_ = physical;
            return;
        }
        Execute(trb, physical);
    }
}

void FakeXhci::Execute(const X::Trb& trb, uint64_t physical)
{
    const uint32_t type = X::TrbTypeOf(trb);
    uint32_t slot = (trb.control & X::TrbControl::SlotMask) >> X::TrbControl::SlotShift;
    uint32_t code = X::CompletionCode::Success;
    if (type == X::TrbType::EnableSlot)
    {
        slot = 0;
        for (uint32_t candidate = 1; candidate <= MaxSlots && candidate <= config_; ++candidate)
        {
            if (enabled_slots_.count(candidate) == 0)
            {
                slot = candidate;
                break;
            }
        }
        if (slot == 0 || enabled_slots_.size() >= slot_limit_)
        {
            slot = 0;
            code = X::CompletionCode::NoSlotsAvailable;
        }
        else
        {
            enabled_slots_.insert(slot);
        }
    }
    else if (type == X::TrbType::DisableSlot)
    {
        if (enabled_slots_.erase(slot) == 0)
        {
            code = X::CompletionCode::SlotNotEnabled;
        }
        else
        {
            disabled_slots_.push_back(slot);
            for (uint32_t dci = 1; dci < 32; ++dci)
            {
                endpoints_.erase(slot << 5 | dci);
            }
            bindings_.erase(slot);
        }
    }
    else if (type == X::TrbType::AddressDevice)
    {
        AddressDevice(slot, X::TrbParameter(trb), &code);
    }
    else if (type == X::TrbType::EvaluateContext)
    {
        EvaluateContext(slot, X::TrbParameter(trb), &code);
    }
    else if (type == X::TrbType::ConfigureEndpoint)
    {
        ConfigureEndpoint(slot, X::TrbParameter(trb), &code);
    }
    else if (type == X::TrbType::ResetEndpoint || type == X::TrbType::StopEndpoint ||
             type == X::TrbType::SetTrDequeue)
    {
        ChangeEndpointState(type, slot, (trb.control & X::TrbControl::EndpointMask) >> X::TrbControl::EndpointShift,
                            X::TrbParameter(trb), &code);
    }
    PostEvent(X::MakeTrb(X::TrbType::CommandCompletionEvent, slot << X::TrbControl::SlotShift,
                     physical, code << X::CompletionShift));
}

void FakeXhci::EvaluateContext(uint32_t slot, uint64_t input_physical, uint32_t* code)
{
    const uint32_t context_bytes = context64_ ? 64 : 32;
    if (enabled_slots_.count(slot) == 0)
    {
        *code = X::CompletionCode::SlotNotEnabled;
        return;
    }
    const uint64_t device_physical = DcbaaEntry(slot);
    volatile uint8_t* input = memory_.Translate(input_physical, 3ULL * context_bytes);
    volatile uint8_t* device =
        device_physical == 0 ? nullptr : memory_.Translate(device_physical, 2ULL * context_bytes);
    if (input == nullptr || device == nullptr)
    {
        ++faults_;
        *code = X::CompletionCode::TrbError;
        return;
    }
    const auto* control = reinterpret_cast<volatile const uint32_t*>(input);
    const auto* ep0 = reinterpret_cast<volatile const uint32_t*>(input + 2ULL * context_bytes);
    if ((Fetch(control + 1) & 2U) != 0)
    {
        const uint32_t packet = Fetch(ep0 + 1) >> 16;
        auto* output = reinterpret_cast<volatile uint32_t*>(device + context_bytes);
        output[1] = (output[1] & 0xFFFFU) | (packet << 16);
        evaluated_.push_back({slot, packet});
    }
}

void FakeXhci::ConfigureEndpoint(uint32_t slot, uint64_t input_physical, uint32_t* code)
{
    const uint32_t context_bytes = context64_ ? 64 : 32;
    if (enabled_slots_.count(slot) == 0)
    {
        *code = X::CompletionCode::SlotNotEnabled;
        return;
    }
    volatile uint8_t* input = memory_.Translate(input_physical, 33ULL * context_bytes);
    if (input == nullptr)
    {
        ++faults_;
        *code = X::CompletionCode::TrbError;
        return;
    }
    const auto* control = reinterpret_cast<volatile const uint32_t*>(input);
    const auto* slot_context = reinterpret_cast<volatile const uint32_t*>(input + context_bytes);
    const uint32_t drop = Fetch(control);
    const uint32_t add = Fetch(control + 1);
    const uint32_t entries = Fetch(slot_context) >> 27;
    SlotUpdate update = {slot, add, {Fetch(slot_context), Fetch(slot_context + 1), Fetch(slot_context + 2)}};
    slot_updates_.push_back(update);
    for (uint32_t dci = 2; dci < 32; ++dci)
    {
        if ((drop & (1U << dci)) != 0)
        {
            endpoints_.erase(slot << 5 | dci);
        }
        if ((add & (1U << dci)) == 0)
        {
            continue;
        }
        const auto* context =
            reinterpret_cast<volatile const uint32_t*>(input + (1ULL + dci) * context_bytes);
        const uint64_t ring = (static_cast<uint64_t>(Fetch(context + 3)) << 32) | Fetch(context + 2);
        Endpoint endpoint;
        endpoint.dequeue = ring & ~0xFULL;
        endpoint.cycle = (ring & 1U) != 0;
        endpoint.address = static_cast<uint8_t>((dci / 2) | ((dci & 1U) != 0 ? 0x80U : 0U));
        const uint32_t type = (Fetch(context + 1) >> 3) & 7U;
        endpoint.bulk = type == 2 || type == 6;
        endpoint.max_packet = Fetch(context + 1) >> 16;
        endpoints_[slot << 5 | dci] = endpoint;
        if ((drop & add & (1U << dci)) != 0)
        {
            reconfigured_.push_back({slot, dci, drop, add, ring});
        }
        configured_.push_back({slot, dci, (Fetch(context + 1) >> 3) & 7U, Fetch(context + 1) >> 16,
                               (Fetch(context) >> 16) & 0xFFU, ring, entries});
    }
}

void FakeXhci::ChangeEndpointState(uint32_t type, uint32_t slot, uint32_t dci, uint64_t parameter,
                                   uint32_t* code)
{
    if (enabled_slots_.count(slot) == 0)
    {
        *code = X::CompletionCode::SlotNotEnabled;
        return;
    }
    const auto found = endpoints_.find(slot << 5 | dci);
    if (found == endpoints_.end())
    {
        *code = X::CompletionCode::ContextStateError;
        return;
    }
    Endpoint& endpoint = found->second;
    if (type == X::TrbType::ResetEndpoint)
    {
        if (endpoint.state != State::Halted)
        {
            *code = X::CompletionCode::ContextStateError;
            return;
        }
        endpoint.state = State::Stopped;
        endpoint.hung = false;
        endpoint.toggle = false;
    }
    else if (type == X::TrbType::StopEndpoint)
    {
        if (endpoint.state != State::Running)
        {
            *code = X::CompletionCode::ContextStateError;
            return;
        }
        endpoint.state = State::Stopped;
        endpoint.hung = false;
        X::Trb trb;
        if (ReadTrb(endpoint.dequeue, &trb) &&
            ((trb.control & X::TrbControl::Cycle) != 0) == endpoint.cycle &&
            X::TrbTypeOf(trb) != X::TrbType::Link)
        {
            PostTransferEvent(slot, dci, endpoint.dequeue, X::CompletionCode::Stopped, 0);
        }
    }
    else
    {
        if (endpoint.state == State::Running)
        {
            *code = X::CompletionCode::ContextStateError;
            return;
        }
        endpoint.dequeue = parameter & ~0xFULL;
        endpoint.cycle = (parameter & 1U) != 0;
        endpoint.hung = false;
        dequeue_sets_.push_back({slot, dci, parameter});
    }
}

const std::vector<FakeXhci::ConfiguredEndpoint>& FakeXhci::Configured() const
{
    return configured_;
}

const std::vector<FakeXhci::Reconfigure>& FakeXhci::Reconfigured() const
{
    return reconfigured_;
}

const std::vector<FakeXhci::DmaWrite>& FakeXhci::DmaWrites() const
{
    return dma_writes_;
}

bool FakeXhci::HostToggle(uint32_t slot, uint32_t dci) const
{
    const auto found = endpoints_.find(slot << 5 | dci);
    return found != endpoints_.end() && found->second.toggle;
}

const std::vector<FakeXhci::SlotUpdate>& FakeXhci::SlotUpdates() const
{
    return slot_updates_;
}

const std::vector<std::pair<uint32_t, uint32_t>>& FakeXhci::Evaluated() const
{
    return evaluated_;
}

const std::vector<FakeXhci::DequeueSet>& FakeXhci::DequeueSets() const
{
    return dequeue_sets_;
}

bool FakeXhci::EndpointState(uint32_t slot, uint32_t dci, State* state) const
{
    const auto found = endpoints_.find(slot << 5 | dci);
    if (found == endpoints_.end())
    {
        return false;
    }
    *state = found->second.state;
    return true;
}

uint32_t FakeXhci::SlotOfPort(uint32_t port) const
{
    for (const auto& [slot, binding] : bindings_)
    {
        if (binding.root_port == port)
        {
            return slot;
        }
    }
    return 0;
}

void FakeXhci::ReleaseHeldCommand()
{
    if (!held_)
    {
        return;
    }
    held_ = false;
    Execute(held_command_, held_trb_);
    ProcessCommands();
}

uint64_t FakeXhci::LastHeldTrb() const
{
    return last_held_trb_;
}

void FakeXhci::SetSlotLimit(uint32_t limit)
{
    slot_limit_ = limit;
}

uint32_t FakeXhci::PortResets(uint32_t port) const
{
    return port_resets_[port - 1];
}

void FakeXhci::AbortCommands()
{
    ++abort_count_;
    if (held_)
    {
        PostEvent(X::MakeTrb(X::TrbType::CommandCompletionEvent, 0, held_trb_,
                             X::CompletionCode::CommandAborted << X::CompletionShift));
        PostEvent(X::MakeTrb(X::TrbType::CommandCompletionEvent, 0, held_trb_,
                             X::CompletionCode::CommandRingStopped << X::CompletionShift));
        held_ = false;
        return;
    }
    PostEvent(X::MakeTrb(X::TrbType::CommandCompletionEvent, 0, command_dequeue_,
                         X::CompletionCode::CommandRingStopped << X::CompletionShift));
}

void FakeXhci::AddressDevice(uint32_t slot, uint64_t input_physical, uint32_t* code)
{
    const uint32_t context_bytes = context64_ ? 64 : 32;
    const uint64_t device_physical = DcbaaEntry(slot);
    if (enabled_slots_.count(slot) == 0)
    {
        *code = X::CompletionCode::SlotNotEnabled;
        return;
    }
    volatile uint8_t* input = memory_.Translate(input_physical, 3ULL * context_bytes);
    volatile uint8_t* device = device_physical == 0 ? nullptr : memory_.Translate(device_physical, 2ULL * context_bytes);
    if (device == nullptr)
    {
        *code = X::CompletionCode::ContextStateError;
        return;
    }
    if (input == nullptr)
    {
        ++faults_;
        *code = X::CompletionCode::TrbError;
        return;
    }
    const auto* control = reinterpret_cast<volatile const uint32_t*>(input);
    const auto* slot_context = reinterpret_cast<volatile const uint32_t*>(input + context_bytes);
    const auto* ep0 = reinterpret_cast<volatile const uint32_t*>(input + 2ULL * context_bytes);
    const uint32_t add_flags = Fetch(control + 1);
    if ((add_flags & 3U) != 3U)
    {
        *code = X::CompletionCode::TrbError;
        return;
    }
    AddressedDevice record = {};
    record.slot = slot;
    record.route = Fetch(slot_context) & 0xFFFFFU;
    record.speed_id = (Fetch(slot_context) >> 20) & 0xFU;
    record.entries = Fetch(slot_context) >> 27;
    record.root_port = (Fetch(slot_context + 1) >> 16) & 0xFFU;
    record.ep0_max_packet = Fetch(ep0 + 1) >> 16;
    record.ep0_ring = (static_cast<uint64_t>(Fetch(ep0 + 3)) << 32) | Fetch(ep0 + 2);
    record.add_flags = add_flags;
    for (uint32_t word = 0; word < 3; ++word)
    {
        record.slot_words[word] = Fetch(slot_context + word);
    }
    addressed_.push_back(record);

    auto* out_slot = reinterpret_cast<volatile uint32_t*>(device);
    auto* out_ep0 = reinterpret_cast<volatile uint32_t*>(device + context_bytes);
    for (uint32_t word = 0; word < 8; ++word)
    {
        out_slot[word] = Fetch(slot_context + word);
        out_ep0[word] = Fetch(ep0 + word);
    }
    out_slot[3] = (2U << 27) | slot;
    out_ep0[0] = (out_ep0[0] & ~7U) | 1U;
    SetEndpointRing(slot, 1, record.ep0_ring & ~0xFULL, (record.ep0_ring & 1U) != 0);
    const std::shared_ptr<ScriptedDevice> attached = Resolve(record.root_port, record.route);
    if (attached != nullptr)
    {
        Speed speed = Speed::Full;
        X::SpeedFromId(static_cast<uint8_t>(record.speed_id), &speed);
        bindings_[slot] = {attached, record.root_port, speed, record.route};
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
    if (endpoint.state == State::Halted)
    {
        return;
    }
    endpoint.state = State::Running;
    const auto binding = bindings_.find(slot);
    if (binding == bindings_.end())
    {
        ProcessPlainTransfers(endpoint, slot, dci);
    }
    else
    {
        ProcessDeviceTransfers(endpoint, binding->second, slot, dci);
    }
}

void FakeXhci::ProcessPlainTransfers(Endpoint& endpoint, uint32_t slot, uint32_t dci)
{
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
            PostTransferEvent(slot, dci, physical, X::CompletionCode::Success, 0);
        }
    }
}

std::shared_ptr<ScriptedDevice> FakeXhci::Resolve(uint32_t root_port, uint32_t route) const
{
    const auto root = port_devices_.find(root_port);
    if (root == port_devices_.end())
    {
        return nullptr;
    }
    std::shared_ptr<ScriptedDevice> device = root->second;
    for (uint32_t shift = 0; (route >> shift) != 0 && device != nullptr; shift += 4)
    {
        const auto hub = std::dynamic_pointer_cast<ScriptedHub>(device);
        device = hub != nullptr ? hub->Child(static_cast<uint8_t>((route >> shift) & 0xFU)) : nullptr;
    }
    return device;
}

bool FakeXhci::DevicePresent(const Binding& binding) const
{
    return Resolve(binding.root_port, binding.route) == binding.device;
}

void FakeXhci::Wake(const std::shared_ptr<ScriptedDevice>& device)
{
    for (auto& [slot, binding] : bindings_)
    {
        if (binding.device != device)
        {
            continue;
        }
        for (uint32_t dci = 2; dci < 32; ++dci)
        {
            if (endpoints_.count(slot << 5 | dci) != 0)
            {
                ProcessTransfers(slot, dci);
            }
        }
    }
}

void FakeXhci::PostTransferEvent(uint32_t slot, uint32_t dci, uint64_t trb, uint32_t code,
                                 uint32_t residue)
{
    PostEvent(X::MakeTrb(X::TrbType::TransferEvent,
                         (dci << X::TrbControl::EndpointShift) | (slot << X::TrbControl::SlotShift),
                         trb, (code << X::CompletionShift) | (residue & X::ResidueMask)));
}

bool FakeXhci::CopyToBuffer(uint64_t physical, const std::vector<uint8_t>& bytes, size_t count)
{
    if (count == 0)
    {
        return true;
    }
    volatile uint8_t* target = memory_.Translate(physical, count);
    if (target == nullptr)
    {
        ++faults_;
        return false;
    }
    dma_writes_.push_back({physical, count});
    for (size_t index = 0; index < count; ++index)
    {
        target[index] = bytes[index];
    }
    return true;
}

uint32_t FakeXhci::ContextMaxPacket(uint32_t slot)
{
    const uint32_t context_bytes = context64_ ? 64 : 32;
    const uint64_t device_physical = DcbaaEntry(slot);
    volatile uint8_t* device =
        device_physical == 0 ? nullptr : memory_.Translate(device_physical, 2ULL * context_bytes);
    if (device == nullptr)
    {
        return 0;
    }
    return Fetch(reinterpret_cast<volatile const uint32_t*>(device + context_bytes) + 1) >> 16;
}

void FakeXhci::ProcessDeviceTransfers(Endpoint& endpoint, Binding& binding, uint32_t slot,
                                      uint32_t dci)
{
    using Outcome = ScriptedDevice::Outcome;
    ScriptedDevice& device = *binding.device;
    for (uint32_t step = 0; step < LoopLimit; ++step)
    {
        if (endpoint.hung)
        {
            return;
        }
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
        transfer_types_.push_back(type);
        const bool in_data = (trb.control & X::TrbControl::DirectionIn) != 0;
        const uint32_t length = trb.status & 0x1FFFFU;
        const bool present = DevicePresent(binding);
        const auto halt = [&](uint32_t code)
        {
            PostTransferEvent(slot, dci, physical, code, length);
            endpoint.state = State::Halted;
        };
        if (type == X::TrbType::SetupStage)
        {
            if (!present)
            {
                halt(X::CompletionCode::UsbTransactionError);
                return;
            }
            const uint64_t packed = X::TrbParameter(trb);
            ControlTd& td = endpoint.control;
            td = {};
            td.active = true;
            td.request = {static_cast<uint8_t>(packed), static_cast<uint8_t>(packed >> 8),
                          static_cast<uint16_t>(packed >> 16), static_cast<uint16_t>(packed >> 32),
                          static_cast<uint16_t>(packed >> 48)};
            td.outcome = device.Control(td.request, &td.response);
            if (td.outcome == Outcome::Hang)
            {
                endpoint.hung = true;
                return;
            }
            if (td.outcome == Outcome::TransactionError)
            {
                halt(X::CompletionCode::UsbTransactionError);
                return;
            }
            endpoint.dequeue += X::TrbSize;
            continue;
        }
        if (type == X::TrbType::DataStage && endpoint.control.active)
        {
            ControlTd& td = endpoint.control;
            if (td.outcome == Outcome::Stall || td.outcome == Outcome::Babble)
            {
                halt(td.outcome == Outcome::Stall ? X::CompletionCode::StallError
                                                  : X::CompletionCode::BabbleDetected);
                return;
            }
            size_t usable = std::min<size_t>(length, td.response.size());
            const uint32_t context_packet = ContextMaxPacket(slot);
            const uint32_t packet = device.MaxPacket0(binding.speed);
            if (in_data)
            {
                if (context_packet != 0 && context_packet < packet && usable > context_packet)
                {
                    halt(X::CompletionCode::BabbleDetected);
                    return;
                }
                if (context_packet > packet && usable > packet)
                {
                    usable = packet;
                }
                const uint64_t buffer = X::TrbParameter(trb);
                CopyToBuffer(buffer, td.response, usable);
                endpoint.dequeue += X::TrbSize;
                if (usable < length && (trb.control & X::TrbControl::InterruptOnShortPacket) != 0)
                {
                    PostTransferEvent(slot, dci, physical, X::CompletionCode::ShortPacket,
                                      static_cast<uint32_t>(length - usable));
                }
                continue;
            }
            const volatile uint8_t* out =
                length == 0 ? nullptr : memory_.Translate(X::TrbParameter(trb), length);
            std::vector<uint8_t> sent;
            for (uint32_t index = 0; out != nullptr && index < length; ++index)
            {
                sent.push_back(static_cast<uint8_t>(out[index]));
            }
            device.RecordData(td.request, std::move(sent));
            endpoint.dequeue += X::TrbSize;
            continue;
        }
        if (type == X::TrbType::StatusStage && endpoint.control.active)
        {
            if (endpoint.control.outcome == Outcome::Stall ||
                endpoint.control.outcome == Outcome::Babble)
            {
                halt(endpoint.control.outcome == Outcome::Stall ? X::CompletionCode::StallError
                                                                : X::CompletionCode::BabbleDetected);
                return;
            }
            endpoint.control.active = false;
            endpoint.dequeue += X::TrbSize;
            if ((trb.control & X::TrbControl::InterruptOnCompletion) != 0)
            {
                PostTransferEvent(slot, dci, physical, X::CompletionCode::Success, 0);
            }
            continue;
        }
        if (type == X::TrbType::Normal && endpoint.bulk)
        {
            if (!ProcessBulkTd(endpoint, binding, slot, dci))
            {
                return;
            }
            continue;
        }
        if (type == X::TrbType::Normal)
        {
            const Outcome outcome = present ? device.EndpointOutcome(endpoint.address)
                                            : Outcome::TransactionError;
            if (outcome != Outcome::Ok)
            {
                halt(outcome == Outcome::Stall      ? X::CompletionCode::StallError
                     : outcome == Outcome::Babble   ? X::CompletionCode::BabbleDetected
                                                    : X::CompletionCode::UsbTransactionError);
                return;
            }
            const bool in = (endpoint.address & 0x80U) != 0;
            if (in)
            {
                std::vector<uint8_t> report;
                if (!device.TakeReport(endpoint.address, &report))
                {
                    return;
                }
                const size_t count = std::min<size_t>(length, report.size());
                CopyToBuffer(X::TrbParameter(trb), report, count);
                endpoint.dequeue += X::TrbSize;
                const bool short_packet = count < length;
                if ((trb.control & X::TrbControl::InterruptOnCompletion) != 0 || short_packet)
                {
                    PostTransferEvent(slot, dci, physical,
                                      short_packet ? X::CompletionCode::ShortPacket
                                                   : X::CompletionCode::Success,
                                      static_cast<uint32_t>(length - count));
                }
                continue;
            }
            endpoint.dequeue += X::TrbSize;
            if ((trb.control & X::TrbControl::InterruptOnCompletion) != 0)
            {
                PostTransferEvent(slot, dci, physical, X::CompletionCode::Success, 0);
            }
            continue;
        }
        endpoint.dequeue += X::TrbSize;
    }
}

bool FakeXhci::ProcessBulkTd(Endpoint& endpoint, Binding& binding, uint32_t slot, uint32_t dci)
{
    using Outcome = ScriptedDevice::Outcome;
    ScriptedDevice& device = *binding.device;
    std::vector<X::Trb> trbs;
    std::vector<uint64_t> where;
    uint64_t at = endpoint.dequeue;
    for (;;)
    {
        X::Trb trb;
        if (!ReadTrb(at, &trb) || ((trb.control & X::TrbControl::Cycle) != 0) != endpoint.cycle)
        {
            return false;
        }
        if (X::TrbTypeOf(trb) != X::TrbType::Normal || trbs.size() >= 8)
        {
            ++faults_;
            return false;
        }
        trbs.push_back(trb);
        where.push_back(at);
        at += X::TrbSize;
        if ((trb.control & X::TrbControl::Chain) == 0)
        {
            break;
        }
    }
    const bool in = (endpoint.address & 0x80U) != 0;
    const uint32_t packet = endpoint.max_packet != 0 ? endpoint.max_packet : 512;
    std::vector<uint32_t> lengths;
    uint32_t total = 0;
    for (const X::Trb& trb : trbs)
    {
        lengths.push_back(trb.status & X::TrbLengthMask);
        total += lengths.back();
    }
    uint32_t remaining = total;
    for (size_t index = 0; index < trbs.size(); ++index)
    {
        const X::Trb& trb = trbs[index];
        const bool last = index + 1 == trbs.size();
        const uint64_t buffer = X::TrbParameter(trb);
        remaining -= lengths[index];
        const uint32_t expected = std::min<uint32_t>(31, (remaining + packet - 1) / packet);
        const bool window = lengths[index] == 0 || (buffer & 0xFFFF) + lengths[index] <= 0x10000;
        const bool ioc = (trb.control & X::TrbControl::InterruptOnCompletion) != 0;
        const bool isp = (trb.control & X::TrbControl::InterruptOnShortPacket) != 0;
        if (!window || ioc != last || (in && !isp) ||
            ((trb.status & X::TdSizeMask) >> X::TdSizeShift) != expected)
        {
            ++faults_;
        }
    }
    const bool present = DevicePresent(binding);
    const auto halt = [&](uint32_t code)
    {
        PostTransferEvent(slot, dci, where[0], code, total);
        endpoint.state = State::Halted;
        return false;
    };
    const auto code_of = [](Outcome outcome)
    {
        return outcome == Outcome::Stall    ? X::CompletionCode::StallError
               : outcome == Outcome::Babble ? X::CompletionCode::BabbleDetected
                                            : X::CompletionCode::UsbTransactionError;
    };
    const auto complete = [&]()
    {
        endpoint.dequeue = at;
        for (size_t index = 0; index < trbs.size(); ++index)
        {
            if ((trbs[index].control & X::TrbControl::InterruptOnCompletion) != 0)
            {
                PostTransferEvent(slot, dci, where[index], X::CompletionCode::Success, 0);
            }
        }
        return true;
    };
    if (!in)
    {
        const Outcome outcome =
            present ? device.EndpointOutcome(endpoint.address) : Outcome::TransactionError;
        if (outcome == Outcome::Hang)
        {
            return false;
        }
        if (outcome == Outcome::Stall || outcome == Outcome::Babble || !present)
        {
            return halt(code_of(present ? outcome : Outcome::TransactionError));
        }
        std::vector<uint8_t> data;
        for (size_t index = 0; index < trbs.size(); ++index)
        {
            const volatile uint8_t* bytes =
                lengths[index] == 0 ? nullptr : memory_.Translate(X::TrbParameter(trbs[index]), lengths[index]);
            for (uint32_t byte = 0; bytes != nullptr && byte < lengths[index]; ++byte)
            {
                data.push_back(static_cast<uint8_t>(bytes[byte]));
            }
        }
        std::vector<uint8_t> accepted;
        bool delivered = false;
        uint32_t offset = 0;
        do
        {
            const uint32_t chunk = std::min<uint32_t>(packet, total - offset);
            if (device.Toggle(endpoint.address) == endpoint.toggle)
            {
                accepted.insert(accepted.end(), data.begin() + offset, data.begin() + offset + chunk);
                device.FlipToggle(endpoint.address);
                delivered = true;
            }
            endpoint.toggle = !endpoint.toggle;
            offset += chunk;
        } while (offset < total);
        if (delivered)
        {
            device.ReceiveBulk(endpoint.address, accepted);
        }
        if (outcome == Outcome::TransactionError)
        {
            return halt(X::CompletionCode::UsbTransactionError);
        }
        return complete();
    }
    std::vector<uint8_t> produced;
    const Outcome outcome = present ? device.ProduceBulk(endpoint.address, total, &produced)
                                    : Outcome::TransactionError;
    if (outcome == Outcome::Hang)
    {
        return false;
    }
    if (outcome != Outcome::Ok)
    {
        return halt(code_of(outcome));
    }
    std::vector<uint8_t> accepted;
    uint32_t offset = 0;
    do
    {
        const auto chunk = static_cast<uint32_t>(std::min<size_t>(packet, produced.size() - offset));
        if (device.Toggle(endpoint.address) == endpoint.toggle)
        {
            accepted.insert(accepted.end(), produced.begin() + offset, produced.begin() + offset + chunk);
            endpoint.toggle = !endpoint.toggle;
        }
        device.FlipToggle(endpoint.address);
        offset += chunk;
    } while (offset < produced.size());
    if (accepted.empty() && !produced.empty())
    {
        return false;
    }
    const auto got = static_cast<uint32_t>(accepted.size());
    uint32_t before = 0;
    for (size_t index = 0; index < trbs.size() && before < got; ++index)
    {
        const uint32_t count = std::min(lengths[index], got - before);
        volatile uint8_t* target = count == 0 ? nullptr : memory_.Translate(X::TrbParameter(trbs[index]), count);
        if (count != 0 && target == nullptr)
        {
            ++faults_;
            break;
        }
        for (uint32_t byte = 0; byte < count; ++byte)
        {
            target[byte] = accepted[before + byte];
        }
        if (count != 0)
        {
            dma_writes_.push_back({X::TrbParameter(trbs[index]), count});
        }
        before += lengths[index];
    }
    if (got == total)
    {
        return complete();
    }
    endpoint.dequeue = at;
    before = 0;
    for (size_t index = 0; index < trbs.size(); ++index)
    {
        const uint32_t end = before + lengths[index];
        if (got <= end)
        {
            PostTransferEvent(slot, dci, where[index], X::CompletionCode::ShortPacket, end - got);
            if (index + 1 != trbs.size())
            {
                PostTransferEvent(slot, dci, where.back(), X::CompletionCode::Success, 0);
            }
            break;
        }
        before = end;
    }
    return true;
}

void FakeXhci::DeliverReport(const std::shared_ptr<ScriptedDevice>& device, uint8_t endpoint,
                             std::vector<uint8_t> report)
{
    device->QueueReport(endpoint, std::move(report));
    for (auto& [slot, binding] : bindings_)
    {
        if (binding.device != device)
        {
            continue;
        }
        const uint32_t dci = (endpoint & 0x0FU) * 2 + ((endpoint & 0x80U) != 0 ? 1 : 0);
        const auto found = endpoints_.find(slot << 5 | dci);
        if (found != endpoints_.end() && found->second.state == State::Running)
        {
            ProcessTransfers(slot, dci);
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
    const bool warm = (value & X::PortSc::WarmReset) != 0;
    if (((value & X::PortSc::Reset) != 0 || warm) && (status & X::PortSc::CurrentConnect) != 0)
    {
        ++port_resets_[port - 1];
        status |= X::PortSc::Enabled | X::PortSc::ResetChange |
                  (warm ? X::PortSc::WarmResetChange : 0);
        status &= ~X::PortSc::LinkStateMask;
        PostPortEvent(port);
    }
}

void FakeXhci::SetEndpointRing(uint32_t slot, uint32_t dci, uint64_t ring, bool cycle)
{
    endpoints_[slot << 5 | dci] = {ring, cycle};
}

void FakeXhci::Connect(uint32_t port, Speed speed, bool trained,
                       std::shared_ptr<ScriptedDevice> device)
{
    port_devices_[port] = device != nullptr ? std::move(device) : MakeKeyboard(speed);
    portsc_[port - 1] = X::PortSc::CurrentConnect | X::PortSc::ConnectChange | X::PortSc::Power |
                        (static_cast<uint32_t>(X::SpeedId(speed)) << X::PortSc::SpeedShift);
    if (trained && PortIsUsb3(port) && speed >= Speed::Super)
    {
        portsc_[port - 1] |= X::PortSc::Enabled;
    }
    PostPortEvent(port);
}

void FakeXhci::Disconnect(uint32_t port)
{
    port_devices_.erase(port);
    uint32_t& status = portsc_[port - 1];
    status &= ~(X::PortSc::CurrentConnect | X::PortSc::Enabled | X::PortSc::SpeedMask);
    status |= X::PortSc::ConnectChange;
    PostPortEvent(port);
}

bool FakeXhci::PortIsUsb3(uint32_t port) const
{
    return usb3_first_ != 0 && port >= usb3_first_;
}

void FakeXhci::BuildProtocols()
{
    protocol_words_.clear();
    struct Range
    {
        uint32_t major;
        uint32_t first;
        uint32_t count;
    };
    std::vector<Range> ranges;
    if (usb3_first_ == 0)
    {
        ranges.push_back({2, 1, ports_});
    }
    else
    {
        if (usb3_first_ > 1)
        {
            ranges.push_back({2, 1, usb3_first_ - 1});
        }
        ranges.push_back({3, usb3_first_, ports_ - usb3_first_ + 1});
    }
    for (size_t index = 0; index < ranges.size(); ++index)
    {
        const uint32_t next = index + 1 == ranges.size() ? 0 : 4;
        protocol_words_.push_back(X::ExtendedCapability::SupportedProtocol | (next << 8) |
                                  (ranges[index].major << 24));
        protocol_words_.push_back(X::ExtendedCapability::ProtocolName);
        protocol_words_.push_back(ranges[index].first | (ranges[index].count << 8));
        protocol_words_.push_back(0);
    }
}

uint32_t FakeXhci::ReadExtended(uint32_t offset)
{
    uint32_t relative = offset - ExtendedBase;
    if (legacy_present_)
    {
        if (relative == 0)
        {
            if (os_owned_ && bios_owned_ && release_after_ >= 0)
            {
                if (legacy_polls_ >= release_after_)
                {
                    bios_owned_ = false;
                }
                else
                {
                    ++legacy_polls_;
                }
            }
            return X::ExtendedCapability::LegacySupport | (2U << 8) |
                   (bios_owned_ ? X::ExtendedCapability::BiosOwned : 0) |
                   (os_owned_ ? X::ExtendedCapability::OsOwned : 0);
        }
        if (relative == 4)
        {
            return smi_enables_ | smi_status_;
        }
        if (relative < 8)
        {
            return 0;
        }
        relative -= 8;
    }
    const size_t index = relative / 4;
    return index < protocol_words_.size() ? protocol_words_[index] : 0;
}

void FakeXhci::WriteExtended(uint32_t offset, uint32_t value)
{
    const uint32_t relative = offset - ExtendedBase;
    if (!legacy_present_)
    {
        return;
    }
    if (relative == 0)
    {
        os_owned_ = (value & X::ExtendedCapability::OsOwned) != 0;
        if ((value & X::ExtendedCapability::BiosOwned) == 0)
        {
            bios_owned_ = false;
        }
    }
    else if (relative == 4)
    {
        smi_enables_ = value & X::ExtendedCapability::SmiEnableMask;
        smi_status_ &= ~(value & X::ExtendedCapability::SmiStatusMask);
    }
}

void FakeXhci::EnableLegacySupport(bool firmware_owns, int release_after_polls)
{
    legacy_present_ = true;
    bios_owned_ = firmware_owns;
    os_owned_ = false;
    release_after_ = release_after_polls;
    legacy_polls_ = 0;
    smi_enables_ = X::ExtendedCapability::SmiEnableMask;
    smi_status_ = X::ExtendedCapability::SmiStatusMask;
}

void FakeXhci::SetUsb3Ports(uint32_t usb3_first)
{
    usb3_first_ = usb3_first;
    BuildProtocols();
}

void FakeXhci::SetScratchpads(uint32_t count)
{
    scratchpads_ = count;
}

void FakeXhci::SetAc64(bool ac64)
{
    ac64_ = ac64;
}

void FakeXhci::SetPageSize(uint32_t page_size_bits)
{
    page_size_bits_ = page_size_bits;
}

void FakeXhci::SetPortPowerControl(bool enabled)
{
    port_power_control_ = enabled;
    for (uint32_t& status : portsc_)
    {
        status = enabled ? status & ~X::PortSc::Power : status | X::PortSc::Power;
    }
}

void FakeXhci::StartRunning()
{
    usbcmd_ |= X::UsbCmd::RunStop;
    usbsts_ &= ~X::UsbSts::HostControllerHalted;
}

void FakeXhci::SetResetDelay(uint32_t polls, bool never_ready)
{
    reset_delay_ = polls;
    never_ready_ = never_ready;
}

void FakeXhci::SetHaltDelay(uint32_t polls, bool never_halts)
{
    halt_delay_ = polls;
    never_halts_ = never_halts;
}

void FakeXhci::HoldNextCommand(uint32_t type)
{
    hold_type_ = type;
}

bool FakeXhci::PostInjectedEvent(const X::Trb& event)
{
    return PostEvent(event);
}

void FakeXhci::SetStatusBits(uint32_t bits)
{
    usbsts_ |= bits;
}

const std::vector<FakeXhci::ResetRecord>& FakeXhci::Resets() const
{
    return resets_;
}

const std::vector<FakeXhci::AddressedDevice>& FakeXhci::Addressed() const
{
    return addressed_;
}

const std::vector<uint32_t>& FakeXhci::DisabledSlots() const
{
    return disabled_slots_;
}

uint32_t FakeXhci::NotReadyAccesses() const
{
    return not_ready_accesses_;
}

uint32_t FakeXhci::AbortCount() const
{
    return abort_count_;
}

uint32_t FakeXhci::SmiEnables() const
{
    return smi_enables_;
}

uint32_t FakeXhci::SmiStatus() const
{
    return smi_status_;
}

bool FakeXhci::OsOwned() const
{
    return os_owned_;
}

bool FakeXhci::BiosOwned() const
{
    return bios_owned_;
}

uint64_t FakeXhci::DcbaaPointer() const
{
    return dcbaap_;
}

uint64_t FakeXhci::DcbaaEntry(uint32_t index)
{
    volatile uint8_t* table = dcbaap_ == 0 ? nullptr : memory_.Translate(dcbaap_ + index * 8ULL, 8);
    if (table == nullptr)
    {
        return 0;
    }
    uint64_t value = 0;
    for (uint32_t byte = 0; byte < 8; ++byte)
    {
        value |= static_cast<uint64_t>(table[byte]) << (byte * 8);
    }
    return value;
}

uint32_t FakeXhci::UsbCommand() const
{
    return usbcmd_;
}

uint32_t FakeXhci::ConfiguredSlots() const
{
    return config_;
}

uint32_t FakeXhci::InterruptModeration() const
{
    return imod_;
}

uint32_t FakeXhci::ErstSize() const
{
    return erst_size_;
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
