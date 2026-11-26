#include "controller.h"

#include "kernel/klogger.h"
#include "kernel/usb/xhci/registers.h"
#include "kernel/usb/xhci/td.h"

#include <algorithm>

namespace FeatherOS::Usb::Xhci
{
namespace
{

constexpr uint32_t SetupBytes = 8;
constexpr uint64_t RecoveryWindowMs = 1000;

constinit KLogger logger;

uint32_t DeviceContextIndex(uint8_t address)
{
    return static_cast<uint32_t>(address & 0x0FU) * 2 + ((address & 0x80U) != 0 ? 1 : 0);
}

Status StatusFromCode(uint32_t code)
{
    switch (code)
    {
    case CompletionCode::UsbTransactionError:
        return Status::TransactionError;
    case CompletionCode::BabbleDetected:
        return Status::Babble;
    case CompletionCode::StallError:
    case CompletionCode::SplitTransactionError:
        return Status::Stall;
    default:
        break;
    }
    return Status::ControllerError;
}

struct SettleState
{
    const std::atomic<bool>* finished;
    const std::atomic<uint32_t>* recovery;
    const std::atomic<bool>* error;
    uint32_t bit;
};

bool Settled(const void* context)
{
    const auto* state = static_cast<const SettleState*>(context);
    return state->finished->load(std::memory_order_acquire) ||
           (state->recovery->load() & state->bit) != 0 || state->error->load();
}

} // namespace

bool Controller::SubmitTransfer(uint32_t slot, Transfer& transfer, const SetupPacket* setup)
{
    const bool control = setup != nullptr;
    const uint32_t dci = control ? 1 : DeviceContextIndex(transfer.endpoint);
    transfer.slot = slot;
    transfer.status = Status::Success;
    transfer.actual = 0;
    transfer.finished.store(false, std::memory_order_release);
    if (control)
    {
        transfer.length = setup->length;
    }
    const auto refuse = [&transfer](Status status)
    {
        transfer.status = status;
        return false;
    };
    if (slot == 0 || slot > MaxSlots || dci < 1 || dci >= ContextIndexes ||
        (!control && (transfer.endpoint & 0x0FU) == 0))
    {
        return refuse(Status::NoDevice);
    }
    if (transfer.length > (control ? MaxTransferBytes : MaxBulkTransferBytes) ||
        (transfer.length != 0 &&
         (transfer.buffer == nullptr || transfer.buffer->size < transfer.length ||
          (control && (transfer.buffer->physical & 0xFFFFU) + transfer.length > 0x10000U))))
    {
        return refuse(Status::ControllerError);
    }
    if (error_pending_.load() || disabled_.load())
    {
        return refuse(Status::ControllerError);
    }
    Sync::SpinLockGuard guard(ring_lock_);
    const EndpointRing& endpoint = endpoint_rings_[slot][dci];
    if (!endpoint.active)
    {
        return refuse(Status::NoDevice);
    }
    if (endpoint.quiescing)
    {
        return refuse(Status::Cancelled);
    }
    if (endpoint.halted)
    {
        return refuse(Status::Stall);
    }
    PendingTransfer* entry = nullptr;
    for (PendingTransfer& candidate : transfers_)
    {
        if (candidate.state == TransferState::Free)
        {
            entry = &candidate;
            break;
        }
    }
    if (entry == nullptr)
    {
        return refuse(Status::ControllerError);
    }
    *entry = {};
    entry->state = TransferState::Active;
    entry->control = control;
    entry->slot = static_cast<uint8_t>(slot);
    entry->dci = static_cast<uint8_t>(dci);
    entry->transfer = &transfer;
    if (control)
    {
        entry->setup = *setup;
    }
    if (!QueueTransferLocked(*entry))
    {
        entry->state = TransferState::Free;
        return refuse(Status::ControllerError);
    }
    return true;
}

bool Controller::QueueTransferLocked(PendingTransfer& entry)
{
    EndpointRing& endpoint = endpoint_rings_[entry.slot][entry.dci];
    const Transfer& transfer = *entry.transfer;
    const uint64_t physical = transfer.length != 0 ? transfer.buffer->physical : 0;
    Trb td[MaxTdTrbs];
    uint32_t count = 0;
    if (entry.control)
    {
        const SetupPacket& setup = entry.setup;
        const bool in = (setup.request_type & 0x80U) != 0;
        const uint32_t transfer_type = setup.length == 0 ? 0 : (in ? 3 : 2);
        const uint64_t packed = static_cast<uint64_t>(setup.request_type) |
                                (static_cast<uint64_t>(setup.request) << 8) |
                                (static_cast<uint64_t>(setup.value) << 16) |
                                (static_cast<uint64_t>(setup.index) << 32) |
                                (static_cast<uint64_t>(setup.length) << 48);
        td[count++] =
            MakeTrb(TrbType::SetupStage,
                    TrbControl::ImmediateData | (transfer_type << TrbControl::TransferTypeShift),
                    packed, SetupBytes);
        if (setup.length != 0)
        {
            td[count++] =
                MakeTrb(TrbType::DataStage,
                        in ? TrbControl::DirectionIn | TrbControl::InterruptOnShortPacket : 0,
                        physical, setup.length);
        }
        const bool status_in = setup.length == 0 || !in;
        td[count++] = MakeTrb(
            TrbType::StatusStage,
            TrbControl::InterruptOnCompletion | (status_in ? TrbControl::DirectionIn : 0), 0, 0);
    }
    else
    {
        const bool in = (transfer.endpoint & 0x80U) != 0;
        count = BuildNormalTd(physical, transfer.length, in,
                              endpoint.descriptor.max_packet_size & 0x7FFU, td);
        if (count == 0)
        {
            return false;
        }
        entry.first_chunk = TdTrbLength(physical, transfer.length, 0);
    }
    uint64_t first = 0;
    if (!endpoint.ring.Enqueue(std::span<const Trb>(td, count), &first))
    {
        return false;
    }
    entry.first = first;
    entry.trbs = static_cast<uint8_t>(count);
    entry.last = first + static_cast<uint64_t>(count - 1) * TrbSize;
    Memory::Dma::ProducerBarrier();
    RingDoorbell(entry.slot, entry.dci);
    return true;
}

Controller::PendingTransfer* Controller::FindTransferLocked(uint32_t slot, uint32_t dci,
                                                            uint64_t pointer)
{
    for (PendingTransfer& entry : transfers_)
    {
        if (entry.state != TransferState::Free && entry.slot == slot && entry.dci == dci &&
            pointer >= entry.first && pointer <= entry.last)
        {
            return &entry;
        }
    }
    return nullptr;
}

Controller::PendingTransfer* Controller::FindFailedLocked(uint32_t slot, uint32_t dci)
{
    for (PendingTransfer& entry : transfers_)
    {
        if (entry.state == TransferState::Failed && entry.slot == slot && entry.dci == dci)
        {
            return &entry;
        }
    }
    return nullptr;
}

Controller::PendingTransfer* Controller::FindWaitingLocked(const Transfer& transfer)
{
    for (PendingTransfer& entry : transfers_)
    {
        if (entry.state != TransferState::Free && entry.transfer == &transfer)
        {
            return &entry;
        }
    }
    return nullptr;
}

void Controller::HandleTransferEvent(const Trb& event, Completions& completions)
{
    const uint32_t slot = (event.control & TrbControl::SlotMask) >> TrbControl::SlotShift;
    const uint32_t dci = (event.control & TrbControl::EndpointMask) >> TrbControl::EndpointShift;
    const uint32_t code = event.status >> CompletionShift;
    const uint32_t residue = event.status & ResidueMask;
    const uint64_t pointer = TrbParameter(event);
    const bool stopped = code == CompletionCode::Stopped ||
                         code == CompletionCode::StoppedLengthInvalid ||
                         code == CompletionCode::StoppedShortPacket;
    bool flagged = false;
    bool log_unknown = false;
    {
        Sync::SpinLockGuard guard(ring_lock_);
        PendingTransfer* entry = slot <= MaxSlots && dci < ContextIndexes
                                     ? FindTransferLocked(slot, dci, pointer)
                                     : nullptr;
        if (entry == nullptr)
        {
            if (!stopped && !unknown_transfer_logged_)
            {
                unknown_transfer_logged_ = true;
                log_unknown = true;
            }
        }
        else if (entry->state == TransferState::Active && !stopped)
        {
            Transfer& transfer = *entry->transfer;
            EndpointRing& endpoint = endpoint_rings_[slot][dci];
            if (code == CompletionCode::Success || code == CompletionCode::ShortPacket)
            {
                const bool short_packet = code == CompletionCode::ShortPacket;
                uint32_t moved = residue < transfer.length ? transfer.length - residue : 0;
                if (!entry->control)
                {
                    const auto index = static_cast<uint32_t>((pointer - entry->first) / TrbSize);
                    const uint32_t before =
                        index == 0 ? 0 : entry->first_chunk + (index - 1) * MaxBulkTransferBytes;
                    const uint32_t size =
                        index == 0 ? entry->first_chunk
                                   : std::min(MaxBulkTransferBytes, transfer.length - before);
                    moved = before + size - std::min(residue, size);
                }
                if (pointer != entry->last)
                {
                    if (short_packet && !entry->short_seen)
                    {
                        entry->actual = moved;
                        entry->short_seen = true;
                    }
                }
                else
                {
                    if (short_packet && !entry->short_seen)
                    {
                        entry->actual = moved;
                        entry->short_seen = true;
                    }
                    transfer.actual = std::min(entry->short_seen ? entry->actual : transfer.length,
                                               transfer.length);
                    transfer.status = !entry->control && entry->short_seen ? Status::ShortPacket
                                                                           : Status::Success;
                    endpoint.ring.Retire(entry->last);
                    entry->state = TransferState::Free;
                    completions.items[completions.count++] = &transfer;
                }
            }
            else
            {
                entry->state = TransferState::Failed;
                entry->code = code;
                endpoint.halted = true;
                if (!endpoint.recovery_needed)
                {
                    endpoint.recovery_needed = true;
                    recovery_mask_[slot].fetch_or(1U << dci);
                    recovery_count_.fetch_add(1);
                }
                flagged = true;
            }
        }
    }
    if (flagged)
    {
        hardware_.WakeWaiters();
    }
    if (log_unknown)
    {
        logger.Warning("USB: a transfer event names no pending transfer; ignored");
    }
}

void Controller::FinishTransfer(Transfer& transfer)
{
    auto* done = transfer.done;
    transfer.finished.store(true, std::memory_order_release);
    hardware_.WakeWaiters();
    if (done != nullptr)
    {
        done(transfer);
    }
}

void Controller::FinishTransfer(Transfer& transfer, Status status)
{
    transfer.status = status;
    transfer.actual = 0;
    FinishTransfer(transfer);
}

void Controller::AbortTransfers(uint32_t slot, uint32_t dci_mask, Status status)
{
    for (;;)
    {
        Transfer* transfer = nullptr;
        {
            Sync::SpinLockGuard guard(ring_lock_);
            for (PendingTransfer& entry : transfers_)
            {
                if (entry.state != TransferState::Free && (slot == 0 || entry.slot == slot) &&
                    ((dci_mask >> entry.dci) & 1U) != 0)
                {
                    transfer = entry.transfer;
                    entry.state = TransferState::Free;
                    break;
                }
            }
        }
        if (transfer == nullptr)
        {
            return;
        }
        FinishTransfer(*transfer, status);
    }
}

bool Controller::AwaitTransfer(Transfer& transfer, uint32_t dci, uint64_t deadline_milliseconds)
{
    const uint32_t slot = transfer.slot;
    const SettleState state = {&transfer.finished, &recovery_mask_[slot], &error_pending_,
                               1U << dci};
    for (;;)
    {
        const bool settled = hardware_.WaitUntil(Settled, &state, deadline_milliseconds);
        if (transfer.finished.load(std::memory_order_acquire))
        {
            return true;
        }
        if (error_pending_.load())
        {
            AbortTransfers(slot, ~0U, Status::ControllerError);
            return transfer.finished.load(std::memory_order_acquire);
        }
        if (!settled)
        {
            Quiesce(slot, dci, &transfer, Status::Timeout, true);
            return transfer.finished.load(std::memory_order_acquire);
        }
        RecoverEndpoint(slot, dci);
    }
}

Status Controller::Control(uint32_t slot, const SetupPacket& setup, Memory::Dma::Buffer& buffer,
                           uint32_t* actual)
{
    *actual = 0;
    if (slot == 0 || slot > MaxSlots || !slots_[slot].in_use)
    {
        return Status::NoDevice;
    }
    Transfer transfer;
    transfer.buffer = &buffer;
    if (!SubmitTransfer(slot, transfer, &setup))
    {
        return transfer.status;
    }
    AwaitTransfer(transfer, 1, hardware_.NowMilliseconds() + ControlTimeoutMs);
    *actual = transfer.actual;
    return transfer.status == Status::ShortPacket ? Status::Success : transfer.status;
}

bool Controller::Submit(uint32_t slot, Transfer& transfer)
{
    return SubmitTransfer(slot, transfer, nullptr);
}

bool Controller::Wait(Transfer& transfer, uint64_t deadline_milliseconds)
{
    const uint32_t dci =
        (transfer.endpoint & 0x0FU) == 0 ? 1 : DeviceContextIndex(transfer.endpoint);
    if (transfer.slot == 0 || transfer.slot > MaxSlots || dci >= ContextIndexes)
    {
        return transfer.finished.load(std::memory_order_acquire);
    }
    return AwaitTransfer(transfer, dci, deadline_milliseconds);
}

bool Controller::DeviceGone(uint32_t slot)
{
    const uint32_t port = slots_[slot].port;
    return port == 0 || (ReadPort(port) & PortSc::CurrentConnect) == 0;
}

uint32_t Controller::RecordRecoveryLocked(uint32_t slot, uint32_t dci)
{
    EndpointRing& endpoint = endpoint_rings_[slot][dci];
    const uint64_t now = hardware_.NowMilliseconds();
    if (endpoint.recoveries == 0 || now - endpoint.recovery_window >= RecoveryWindowMs)
    {
        endpoint.recovery_window = now;
        endpoint.recoveries = 0;
    }
    return ++endpoint.recoveries;
}

void Controller::RecoverEndpoint(uint32_t slot, uint32_t dci)
{
    if (slot == 0 || slot > MaxSlots || dci == 0 || dci >= ContextIndexes)
    {
        return;
    }
    uint64_t last = 0;
    uint32_t code = 0;
    uint32_t attempts = 0;
    uint8_t address = 0;
    bool bulk = false;
    {
        Sync::SpinLockGuard guard(ring_lock_);
        EndpointRing& endpoint = endpoint_rings_[slot][dci];
        if (!endpoint.recovery_needed)
        {
            return;
        }
        endpoint.recovery_needed = false;
        recovery_mask_[slot].fetch_and(~(1U << dci));
        recovery_count_.fetch_sub(1);
        const PendingTransfer* failed = FindFailedLocked(slot, dci);
        if (failed == nullptr)
        {
            return;
        }
        last = failed->last;
        code = failed->code;
        attempts = failed->attempts;
        address = endpoint.address;
        bulk = endpoint.descriptor.type == EndpointType::Bulk;
    }
    const bool gone = DeviceGone(slot);
    const uint32_t target = (dci << TrbControl::EndpointShift) | (slot << TrbControl::SlotShift);
    bool restored = false;
    if (!gone)
    {
        restored =
            Command(MakeTrb(TrbType::ResetEndpoint, target, 0, 0)).status == CommandStatus::Success;
        uint64_t pointer = 0;
        bool known;
        {
            Sync::SpinLockGuard guard(ring_lock_);
            known = endpoint_rings_[slot][dci].ring.PointerAfter(last, &pointer);
        }
        restored = restored && known &&
                   Command(MakeTrb(TrbType::SetTrDequeue, target, pointer, 0)).status ==
                       CommandStatus::Success;
    }
    if (restored && !bulk && code == CompletionCode::UsbTransactionError &&
        attempts < TransactionRetries)
    {
        Sync::SpinLockGuard guard(ring_lock_);
        PendingTransfer* entry = FindFailedLocked(slot, dci);
        if (entry != nullptr)
        {
            EndpointRing& endpoint = endpoint_rings_[slot][dci];
            endpoint.ring.Retire(last);
            endpoint.halted = false;
            ++entry->attempts;
            entry->state = TransferState::Active;
            if (QueueTransferLocked(*entry))
            {
                return;
            }
            entry->state = TransferState::Failed;
        }
    }
    const Status status = gone ? Status::NoDevice : StatusFromCode(code);
    Transfer* transfer = nullptr;
    uint32_t recent = 0;
    {
        Sync::SpinLockGuard guard(ring_lock_);
        EndpointRing& endpoint = endpoint_rings_[slot][dci];
        PendingTransfer* entry = FindFailedLocked(slot, dci);
        if (entry != nullptr)
        {
            transfer = entry->transfer;
            entry->state = TransferState::Free;
        }
        endpoint.ring.Retire(last);
        endpoint.halted = !restored;
        recent = RecordRecoveryLocked(slot, dci);
    }
    if (restored && dci != 1)
    {
        ClearEndpointFeature(slot, address);
    }
    if (restored)
    {
        Sync::SpinLockGuard guard(ring_lock_);
        for (const PendingTransfer& entry : transfers_)
        {
            if (entry.state == TransferState::Active && entry.slot == slot && entry.dci == dci)
            {
                RingDoorbell(slot, dci);
                break;
            }
        }
    }
    if (transfer != nullptr)
    {
        FinishTransfer(*transfer, status);
    }
    if (recovery_handler_ != nullptr)
    {
        recovery_handler_(recovery_context_, slot, address, recent, status);
    }
}

bool Controller::ClearEndpointFeature(uint32_t slot, uint8_t address)
{
    constexpr uint8_t StandardToEndpoint = 0x02;
    constexpr uint8_t ClearFeature = 1;
    constexpr uint16_t EndpointHalt = 0;
    Memory::Dma::Buffer none{};
    uint32_t actual = 0;
    return Control(slot, {StandardToEndpoint, ClearFeature, EndpointHalt, address, 0}, none,
                   &actual) == Status::Success;
}

void Controller::Quiesce(uint32_t slot, uint32_t dci, Transfer* waited, Status waited_status,
                         bool resume)
{
    {
        Sync::SpinLockGuard guard(ring_lock_);
        endpoint_rings_[slot][dci].quiescing = true;
    }
    const uint32_t target = (dci << TrbControl::EndpointShift) | (slot << TrbControl::SlotShift);
    const CommandResult stopped = Command(MakeTrb(TrbType::StopEndpoint, target, 0, 0));
    if (stopped.status == CommandStatus::Failed &&
        stopped.code == CompletionCode::ContextStateError)
    {
        Command(MakeTrb(TrbType::ResetEndpoint, target, 0, 0));
    }
    uint64_t pointer;
    {
        Sync::SpinLockGuard guard(ring_lock_);
        pointer = endpoint_rings_[slot][dci].ring.Drain();
    }
    if (stopped.status != CommandStatus::Unavailable)
    {
        Command(MakeTrb(TrbType::SetTrDequeue, target, pointer, 0));
    }
    if (waited != nullptr)
    {
        Transfer* found = nullptr;
        {
            Sync::SpinLockGuard guard(ring_lock_);
            PendingTransfer* entry = FindWaitingLocked(*waited);
            if (entry != nullptr)
            {
                found = entry->transfer;
                entry->state = TransferState::Free;
            }
        }
        if (found != nullptr)
        {
            FinishTransfer(*found, waited_status);
        }
    }
    AbortTransfers(slot, 1U << dci, Status::Cancelled);
    Sync::SpinLockGuard guard(ring_lock_);
    EndpointRing& endpoint = endpoint_rings_[slot][dci];
    endpoint.quiescing = !resume;
    endpoint.halted = false;
    if (endpoint.recovery_needed)
    {
        endpoint.recovery_needed = false;
        recovery_mask_[slot].fetch_and(~(1U << dci));
        recovery_count_.fetch_sub(1);
    }
}

bool Controller::ResetEndpoint(uint32_t slot, uint8_t endpoint)
{
    const uint32_t dci = DeviceContextIndex(endpoint);
    if (slot == 0 || slot > MaxSlots || (endpoint & 0x0FU) == 0 || !slots_[slot].in_use)
    {
        return false;
    }
    RecoverEndpoint(slot, dci);
    bool halted;
    {
        Sync::SpinLockGuard guard(ring_lock_);
        halted = endpoint_rings_[slot][dci].active && endpoint_rings_[slot][dci].halted;
    }
    if (halted)
    {
        Quiesce(slot, dci, nullptr, Status::Cancelled, true);
    }
    Sync::SpinLockGuard guard(ring_lock_);
    return endpoint_rings_[slot][dci].active && !endpoint_rings_[slot][dci].halted;
}

bool Controller::CancelEndpoint(uint32_t slot, uint8_t endpoint)
{
    const uint32_t dci = DeviceContextIndex(endpoint);
    if (slot == 0 || slot > MaxSlots || (endpoint & 0x0FU) == 0 || !slots_[slot].in_use)
    {
        return false;
    }
    {
        Sync::SpinLockGuard guard(ring_lock_);
        if (!endpoint_rings_[slot][dci].active)
        {
            return false;
        }
    }
    Quiesce(slot, dci, nullptr, Status::Cancelled, true);
    return true;
}

bool Controller::ResetSequence(uint32_t slot, uint32_t dci)
{
    EndpointDescriptor descriptor{};
    uint64_t pointer = 0;
    {
        Sync::SpinLockGuard guard(ring_lock_);
        EndpointRing& endpoint = endpoint_rings_[slot][dci];
        if (!endpoint.active)
        {
            return false;
        }
        descriptor = endpoint.descriptor;
        pointer = endpoint.ring.Drain();
    }
    Slot& entry = slots_[slot];
    ClearInput(slot);
    Memory::Dma::ConsumerBarrier();
    const auto* output = reinterpret_cast<const volatile uint32_t*>(entry.device.address +
                                                                    layout_.SlotOffset(false));
    auto* input =
        reinterpret_cast<volatile uint32_t*>(entry.input.address + layout_.SlotOffset(true));
    for (uint32_t word = 0; word < layout_.ContextSize() / 4; ++word)
    {
        input[word] = output[word];
    }
    WriteEndpoint(layout_, entry.input.address, descriptor, entry.speed, pointer);
    SetInputFlags(layout_, entry.input.address, 1U << dci, (1U << dci) | 1U);
    return Command(MakeTrb(TrbType::ConfigureEndpoint, slot << TrbControl::SlotShift,
                           entry.input.physical, 0))
               .status == CommandStatus::Success;
}

bool Controller::ClearHalt(uint32_t slot, uint8_t endpoint)
{
    const uint32_t dci = DeviceContextIndex(endpoint);
    if (slot == 0 || slot > MaxSlots || (endpoint & 0x0FU) == 0 || !slots_[slot].in_use)
    {
        return false;
    }
    bool halted;
    {
        Sync::SpinLockGuard guard(ring_lock_);
        EndpointRing& ring = endpoint_rings_[slot][dci];
        if (!ring.active)
        {
            return false;
        }
        ring.quiescing = true;
        halted = ring.halted;
    }
    const uint32_t target = (dci << TrbControl::EndpointShift) | (slot << TrbControl::SlotShift);
    bool reset;
    if (halted)
    {
        reset =
            Command(MakeTrb(TrbType::ResetEndpoint, target, 0, 0)).status == CommandStatus::Success;
        uint64_t pointer;
        {
            Sync::SpinLockGuard guard(ring_lock_);
            pointer = endpoint_rings_[slot][dci].ring.Drain();
        }
        reset = reset && Command(MakeTrb(TrbType::SetTrDequeue, target, pointer, 0)).status ==
                             CommandStatus::Success;
    }
    else
    {
        Command(MakeTrb(TrbType::StopEndpoint, target, 0, 0));
        reset = ResetSequence(slot, dci);
    }
    AbortTransfers(slot, 1U << dci, Status::Cancelled);
    {
        Sync::SpinLockGuard guard(ring_lock_);
        EndpointRing& ring = endpoint_rings_[slot][dci];
        ring.halted = !reset;
        if (ring.recovery_needed)
        {
            ring.recovery_needed = false;
            recovery_mask_[slot].fetch_and(~(1U << dci));
            recovery_count_.fetch_sub(1);
        }
    }
    const bool cleared = reset && ClearEndpointFeature(slot, endpoint);
    Sync::SpinLockGuard guard(ring_lock_);
    endpoint_rings_[slot][dci].quiescing = false;
    return cleared;
}

void Controller::StopEndpoints(uint32_t slot)
{
    if (slot == 0 || slot > MaxSlots || !slots_[slot].in_use)
    {
        return;
    }
    for (uint32_t dci = 1; dci < ContextIndexes; ++dci)
    {
        bool active;
        {
            Sync::SpinLockGuard guard(ring_lock_);
            active = endpoint_rings_[slot][dci].active;
        }
        if (active)
        {
            Quiesce(slot, dci, nullptr, Status::Cancelled, false);
        }
    }
}

void Controller::ServiceEndpoints()
{
    if (recovery_count_.load() == 0)
    {
        return;
    }
    for (uint32_t slot = 1; slot <= MaxSlots; ++slot)
    {
        for (uint32_t pending = recovery_mask_[slot].load(); pending != 0;
             pending = recovery_mask_[slot].load())
        {
            const auto dci = static_cast<uint32_t>(__builtin_ctz(pending));
            // RecoverEndpoint clears the bit under ring_lock_; a second clear here would lose a
            // failure flagged while the endpoint is being recovered.
            RecoverEndpoint(slot, dci);
        }
    }
}

void Controller::SetRecoveryHandler(RecoveryHandler handler, void* context)
{
    recovery_handler_ = handler;
    recovery_context_ = context;
}

uint32_t Controller::RecentRecoveries(uint32_t slot, uint8_t endpoint)
{
    const uint32_t dci = DeviceContextIndex(endpoint);
    if (slot == 0 || slot > MaxSlots || dci >= ContextIndexes)
    {
        return 0;
    }
    Sync::SpinLockGuard guard(ring_lock_);
    const EndpointRing& entry = endpoint_rings_[slot][dci];
    return hardware_.NowMilliseconds() - entry.recovery_window < RecoveryWindowMs ? entry.recoveries
                                                                                  : 0;
}

} // namespace FeatherOS::Usb::Xhci
