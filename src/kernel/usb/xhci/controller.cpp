#include "controller.h"

#include "kernel/klogger.h"
#include "kernel/string/format.h"
#include "kernel/usb/xhci/registers.h"

namespace FeatherOS::Usb::Xhci
{
namespace
{

constexpr uint32_t HandoffTimeoutMs = 1000;
constexpr uint32_t HaltTimeoutMs = 32;
constexpr uint32_t ResetTimeoutMs = 1000;
constexpr uint32_t IntelResetTimeoutMs = 10000;
constexpr uint32_t StartTimeoutMs = 100;
constexpr uint32_t CommandTimeoutMs = 5000;
constexpr uint32_t AbortTimeoutMs = 1000;
constexpr uint32_t DebounceMs = 100;
constexpr uint32_t PortResetTimeoutMs = 500;
constexpr uint32_t InterruptModeration = 4000;
constexpr uint64_t FourGiB = 1ULL << 32;
constexpr uint64_t PageBytes = 4096;
constexpr uint16_t IntelVendor = 0x8086;
constexpr uint16_t IntelPortSwitch = 0xD0;
constexpr uint16_t IntelUsb2Routing = 0xD4;
constexpr uint16_t IntelUsb3Enable = 0xD8;
constexpr uint16_t IntelUsb3Routing = 0xDC;
constexpr uint32_t UsbStatusErrors = UsbSts::HostControllerError | UsbSts::HostSystemError;
constexpr uint32_t U0 = 0;

constinit KLogger logger;

bool IntelWithEhci(uint16_t device)
{
    // Panther Point, Lynx Point and its low-power variant, Wildcat Point.
    return device == 0x1E31 || device == 0x8C31 || device == 0x9C31 || device == 0x8CB1 ||
           device == 0x9CB1;
}

class Text
{
public:
    Text(char* destination, size_t capacity) : out_(destination), capacity_(capacity) {}

    void Put(char value)
    {
        if (length_ + 1 < capacity_)
        {
            out_[length_] = value;
        }
        ++length_;
    }

    void Words(const char* text)
    {
        while (*text != '\0')
        {
            Put(*text++);
        }
    }

    void Hex(uint32_t value, uint32_t digits)
    {
        constexpr char Digits[] = "0123456789abcdef";
        for (uint32_t shift = digits * 4; shift > 0; shift -= 4)
        {
            Put(Digits[(value >> (shift - 4)) & 0xFU]);
        }
    }

    void Decimal(uint32_t value)
    {
        char reversed[10];
        uint32_t count = 0;
        do
        {
            reversed[count++] = static_cast<char>('0' + value % 10);
            value /= 10;
        } while (value != 0);
        while (count > 0)
        {
            Put(reversed[--count]);
        }
    }

    bool Finish()
    {
        if (capacity_ == 0 || length_ + 1 > capacity_)
        {
            return false;
        }
        out_[length_] = '\0';
        return true;
    }

private:
    char* out_;
    size_t capacity_;
    size_t length_ = 0;
};

void AppendAddress(Text& text, const Pci::Function& function)
{
    text.Hex(function.address.bus, 2);
    text.Put(':');
    text.Hex(function.address.device, 2);
    text.Put('.');
    text.Hex(function.address.function, 1);
}

void LogRefusal(const Pci::Function& function, const char* reason)
{
    char line[96];
    Text text(line, sizeof(line));
    text.Words("USB: xHCI at ");
    AppendAddress(text, function);
    text.Words(" refused: ");
    text.Words(reason);
    if (text.Finish())
    {
        logger.Warning(line);
    }
}

const char* SpeedName(Speed speed)
{
    switch (speed)
    {
    case Speed::Low:
        return "low-speed";
    case Speed::Full:
        return "full-speed";
    case Speed::High:
        return "high-speed";
    case Speed::Super:
        return "super-speed";
    case Speed::SuperPlus:
        return "super-speed+";
    }
    return "unknown-speed";
}

const char* InterruptName(InterruptKind kind)
{
    switch (kind)
    {
    case InterruptKind::MsiX:
        return "MSI-X";
    case InterruptKind::Msi:
        return "MSI";
    case InterruptKind::Intx:
        return "INTx";
    case InterruptKind::Polling:
        break;
    }
    return "polling";
}

uint32_t ControlEndpointPacketSize(Speed speed)
{
    switch (speed)
    {
    case Speed::Low:
    case Speed::Full:
        return 8;
    case Speed::High:
        return 64;
    case Speed::Super:
    case Speed::SuperPlus:
        break;
    }
    return 512;
}

void FreeBuffer(Hardware& hardware, Memory::Dma::Buffer& buffer)
{
    if (buffer.size != 0)
    {
        hardware.FreeDma(buffer);
    }
    buffer = {};
}

volatile uint64_t* Table(const Memory::Dma::Buffer& buffer)
{
    return reinterpret_cast<volatile uint64_t*>(buffer.address);
}

} // namespace

Controller::Controller(Hardware& hardware, const Pci::Function& function)
    : hardware_(hardware), function_(function), layout_(false)
{
}

Controller::~Controller()
{
    for (uint32_t slot = 1; slot <= MaxSlots; ++slot)
    {
        if (slots_[slot].in_use)
        {
            ReleaseSlot(slot);
        }
    }
    FreeTables();
}

Hardware& Controller::Machine()
{
    return hardware_;
}

uint32_t Controller::ReadOperational(uint32_t reg)
{
    return hardware_.Read32(operational_ + reg);
}

void Controller::WriteOperational(uint32_t reg, uint32_t value)
{
    hardware_.Write32(operational_ + reg, value);
}

uint32_t Controller::ReadPort(uint32_t port)
{
    return ReadOperational(PortRegister(port, Op::PortSc));
}

void Controller::WritePort(uint32_t port, uint32_t value)
{
    WriteOperational(PortRegister(port, Op::PortSc), value);
}

uint32_t Controller::RuntimeOffset(uint32_t reg) const
{
    return runtime_ + reg;
}

bool Controller::WaitFor(uint32_t offset, uint32_t mask, uint32_t value,
                         uint32_t timeout_milliseconds)
{
    const uint64_t deadline = hardware_.NowMilliseconds() + timeout_milliseconds;
    for (;;)
    {
        if ((hardware_.Read32(offset) & mask) == value)
        {
            return true;
        }
        if (hardware_.NowMilliseconds() >= deadline)
        {
            return false;
        }
        hardware_.Pause(1);
    }
}

bool Controller::Initialize(bool allow_interrupts)
{
    allow_interrupts_ = allow_interrupts;
    if (!Setup())
    {
        if (interrupts_chosen_)
        {
            WriteOperational(Op::UsbCmd, 0);
            WaitFor(operational_ + Op::UsbSts, UsbSts::HostControllerHalted,
                    UsbSts::HostControllerHalted, HaltTimeoutMs);
        }
        FreeTables();
        return false;
    }
    initialized_ = true;
    return true;
}

bool Controller::Setup()
{
    intel_ = function_.vendor == IntelVendor;
    RouteIntelPorts();

    const uint32_t length = hardware_.Read32(Cap::Length);
    operational_ = length & 0xFFU;
    const uint32_t hcs1 = hardware_.Read32(Cap::HcsParams1);
    const uint32_t hcs2 = hardware_.Read32(Cap::HcsParams2);
    const uint32_t hcc1 = hardware_.Read32(Cap::HccParams1);
    doorbells_ = hardware_.Read32(Cap::DoorbellOffset) & ~3U;
    runtime_ = hardware_.Read32(Cap::RuntimeOffset) & ~0x1FU;
    ac64_ = (hcc1 & HccParams1::Ac64) != 0;
    power_control_ = (hcc1 & HccParams1::PortPowerControl) != 0;
    layout_ = ContextLayout((hcc1 & HccParams1::ContextSize64) != 0);
    extended_ = (hcc1 >> HccParams1::ExtendedCapabilityShift) * 4;
    max_slots_ = hcs1 & HcsParams1::MaxSlotsMask;
    max_ports_ = hcs1 >> HcsParams1::MaxPortsShift;
    scratchpads_ = ScratchpadCount(hcs2);
    if (operational_ < 0x20 || max_slots_ == 0 || max_ports_ == 0 || doorbells_ == 0 ||
        runtime_ == 0)
    {
        LogRefusal(function_, "the capability registers are not valid");
        return false;
    }
    if (max_slots_ > MaxSlots)
    {
        max_slots_ = MaxSlots;
    }
    if (max_ports_ > MaxPorts)
    {
        max_ports_ = MaxPorts;
    }

    WalkCapabilities();
    HandOff();
    if (!Halt())
    {
        LogRefusal(function_, "the controller did not halt");
        return false;
    }
    if (!Reset())
    {
        LogRefusal(function_, "the controller did not reset");
        return false;
    }
    if ((ReadOperational(Op::PageSize) & 1U) == 0)
    {
        LogRefusal(function_, "it does not support 4 KiB pages");
        return false;
    }
    if (!AllocateTables())
    {
        LogRefusal(function_, "ENOMEM, no memory the controller can reach");
        return false;
    }
    if (!Program())
    {
        LogRefusal(function_, "the controller did not start");
        return false;
    }
    ReadProtocols();
    QueueConnectedPorts();
    return true;
}

void Controller::RouteIntelPorts()
{
    if (function_.vendor != IntelVendor || !IntelWithEhci(function_.device))
    {
        return;
    }
    hardware_.WriteConfig32(IntelUsb3Enable, hardware_.ReadConfig32(IntelUsb3Routing));
    hardware_.WriteConfig32(IntelPortSwitch, hardware_.ReadConfig32(IntelUsb2Routing));
}

void Controller::WalkCapabilities()
{
    legacy_ = 0;
    uint32_t offset = extended_;
    for (uint32_t step = 0; offset != 0 && step < ExtendedCapability::MaximumSteps; ++step)
    {
        const uint32_t header = hardware_.Read32(offset);
        if ((header & 0xFFU) == ExtendedCapability::LegacySupport && legacy_ == 0)
        {
            legacy_ = offset;
        }
        const uint32_t next = (header >> 8) & 0xFFU;
        if (next == 0)
        {
            break;
        }
        offset += next * 4;
    }
}

void Controller::HandOff()
{
    if (legacy_ == 0)
    {
        return;
    }
    uint32_t support = hardware_.Read32(legacy_);
    const bool firmware_owns = (support & ExtendedCapability::BiosOwned) != 0;
    hardware_.Write32(legacy_, support | ExtendedCapability::OsOwned);
    if (firmware_owns && !WaitFor(legacy_, ExtendedCapability::BiosOwned, 0, HandoffTimeoutMs))
    {
        support = hardware_.Read32(legacy_);
        hardware_.Write32(legacy_,
                          (support & ~ExtendedCapability::BiosOwned) | ExtendedCapability::OsOwned);
        logger.Warning("USB: handoff from firmware forced");
    }
    const uint32_t control = hardware_.Read32(legacy_ + ExtendedCapability::LegacyControlStatus);
    hardware_.Write32(legacy_ + ExtendedCapability::LegacyControlStatus,
                      (control & ~ExtendedCapability::SmiEnableMask) |
                          ExtendedCapability::SmiStatusMask);
}

bool Controller::Halt()
{
    const uint32_t command = ReadOperational(Op::UsbCmd);
    if ((command & UsbCmd::RunStop) != 0)
    {
        WriteOperational(Op::UsbCmd, command & ~UsbCmd::RunStop);
    }
    return WaitFor(operational_ + Op::UsbSts, UsbSts::HostControllerHalted,
                   UsbSts::HostControllerHalted, HaltTimeoutMs);
}

bool Controller::Reset()
{
    WriteOperational(Op::UsbCmd, UsbCmd::HostControllerReset);
    // Intel controllers answer nothing for the first millisecond after a reset.
    hardware_.Pause(1);
    const uint32_t timeout = intel_ ? IntelResetTimeoutMs : ResetTimeoutMs;
    const uint64_t deadline = hardware_.NowMilliseconds() + timeout;
    for (;;)
    {
        if ((ReadOperational(Op::UsbCmd) & UsbCmd::HostControllerReset) == 0 &&
            (ReadOperational(Op::UsbSts) & UsbSts::ControllerNotReady) == 0)
        {
            return true;
        }
        if (hardware_.NowMilliseconds() >= deadline)
        {
            return false;
        }
        hardware_.Pause(1);
    }
}

bool Controller::AllocateTables()
{
    const uint64_t below = ac64_ ? UINT64_MAX : FourGiB;
    if (!hardware_.AllocateDma(MaxSlots * 8 + 8, below, &dcbaa_) ||
        !hardware_.AllocateDma(RingBytes, below, &command_buffer_) ||
        !hardware_.AllocateDma(RingBytes, below, &event_buffer_) ||
        !hardware_.AllocateDma(16, below, &segment_table_))
    {
        return false;
    }
    if (scratchpads_ != 0)
    {
        if (!hardware_.AllocateDma(static_cast<uint64_t>(scratchpads_) * 8, below,
                                   &scratchpad_array_) ||
            !hardware_.AllocateDma(static_cast<uint64_t>(scratchpads_) * PageBytes, below,
                                   &scratchpad_pages_))
        {
            return false;
        }
        volatile uint64_t* array = Table(scratchpad_array_);
        for (uint32_t index = 0; index < scratchpads_; ++index)
        {
            array[index] = scratchpad_pages_.physical + static_cast<uint64_t>(index) * PageBytes;
        }
        Table(dcbaa_)[0] = scratchpad_array_.physical;
    }
    return true;
}

void Controller::FreeTables()
{
    FreeBuffer(hardware_, scratchpad_pages_);
    FreeBuffer(hardware_, scratchpad_array_);
    FreeBuffer(hardware_, segment_table_);
    FreeBuffer(hardware_, event_buffer_);
    FreeBuffer(hardware_, command_buffer_);
    FreeBuffer(hardware_, dcbaa_);
}

bool Controller::Program()
{
    WriteOperational(Op::Config, max_slots_);
    hardware_.Write64(operational_ + Op::Dcbaap, dcbaa_.physical);
    {
        Sync::SpinLockGuard guard(ring_lock_);
        if (!commands_.Initialize(command_buffer_, TrbType::NoOpCommand))
        {
            return false;
        }
        for (uint32_t index = 0; index < PendingCommands; ++index)
        {
            pending_[index].used = false;
            pending_[index].abandoned = false;
        }
        hardware_.Write64(operational_ + Op::CommandRing,
                          commands_.Physical() | CommandRingControl::RingCycleState);
    }
    for (uint32_t index = 0; index < PendingCommands; ++index)
    {
        done_[index].store(false);
    }

    if (!events_.Initialize(event_buffer_))
    {
        return false;
    }
    volatile uint32_t* entry = reinterpret_cast<volatile uint32_t*>(segment_table_.address);
    entry[0] = static_cast<uint32_t>(event_buffer_.physical);
    entry[1] = static_cast<uint32_t>(event_buffer_.physical >> 32);
    entry[2] = RingTrbs;
    entry[3] = 0;
    Memory::Dma::ProducerBarrier();
    hardware_.Write32(RuntimeOffset(InterrupterRegister(0, Runtime::ErstSize)), 1);
    hardware_.Write64(RuntimeOffset(InterrupterRegister(0, Runtime::Erdp)), events_.Physical());
    hardware_.Write64(RuntimeOffset(InterrupterRegister(0, Runtime::ErstBase)),
                      segment_table_.physical);
    hardware_.Write32(RuntimeOffset(InterrupterRegister(0, Runtime::Imod)), InterruptModeration);

    if (!interrupts_chosen_)
    {
        interrupts_ = hardware_.SetUpInterrupts(allow_interrupts_, OnInterrupt, this);
        interrupts_chosen_ = true;
        if (interrupts_ == InterruptKind::Polling)
        {
            logger.Info("USB: interrupts by polling");
        }
    }
    hardware_.Write32(RuntimeOffset(InterrupterRegister(0, Runtime::Iman)),
                      Iman::InterruptPending | Iman::InterruptEnable);
    uint32_t command = UsbCmd::RunStop | UsbCmd::HostSystemErrorEnable;
    if (interrupts_ != InterruptKind::Polling)
    {
        command |= UsbCmd::InterrupterEnable;
    }
    WriteOperational(Op::UsbCmd, command);
    if (!WaitFor(operational_ + Op::UsbSts, UsbSts::HostControllerHalted, 0, StartTimeoutMs))
    {
        return false;
    }
    if (power_control_)
    {
        for (uint32_t port = 1; port <= max_ports_; ++port)
        {
            const uint32_t status = ReadPort(port);
            if ((status & PortSc::Power) == 0)
            {
                WritePort(port, PortscWriteValue(status, 0) | PortSc::Power);
            }
        }
    }
    return true;
}

void Controller::ReadProtocols()
{
    for (uint32_t port = 0; port <= MaxPorts; ++port)
    {
        port_major_[port] = 2;
        port_slot_type_[port] = 0;
    }
    uint32_t offset = extended_;
    for (uint32_t step = 0; offset != 0 && step < ExtendedCapability::MaximumSteps; ++step)
    {
        const uint32_t header = hardware_.Read32(offset);
        if ((header & 0xFFU) == ExtendedCapability::SupportedProtocol &&
            hardware_.Read32(offset + 4) == ExtendedCapability::ProtocolName)
        {
            const uint32_t major = header >> 24;
            const uint32_t ports = hardware_.Read32(offset + 8);
            const uint32_t first = ports & 0xFFU;
            const uint32_t count = (ports >> 8) & 0xFFU;
            const uint32_t slot_type = hardware_.Read32(offset + 12) & 0x1FU;
            for (uint32_t port = first; port < first + count && port <= MaxPorts; ++port)
            {
                if (port != 0)
                {
                    port_major_[port] = static_cast<uint8_t>(major);
                    port_slot_type_[port] = static_cast<uint8_t>(slot_type);
                }
            }
        }
        const uint32_t next = (header >> 8) & 0xFFU;
        if (next == 0)
        {
            break;
        }
        offset += next * 4;
    }
}

void Controller::QueueConnectedPorts()
{
    for (uint32_t port = 1; port <= max_ports_; ++port)
    {
        const uint32_t status = ReadPort(port);
        if ((status & (PortSc::CurrentConnect | PortSc::ChangeMask)) != 0)
        {
            SetPortWork(port);
        }
    }
}

void Controller::SetPortWork(uint32_t port)
{
    if (port == 0 || port > max_ports_)
    {
        return;
    }
    Sync::SpinLockGuard guard(work_lock_);
    uint32_t& word = port_work_[port / 32];
    const uint32_t bit = 1U << (port % 32);
    if ((word & bit) == 0)
    {
        word |= bit;
        port_work_count_.fetch_add(1);
    }
}

bool Controller::TakePortWork(uint32_t* port)
{
    Sync::SpinLockGuard guard(work_lock_);
    for (uint32_t index = 0; index < sizeof(port_work_) / sizeof(port_work_[0]); ++index)
    {
        if (port_work_[index] == 0)
        {
            continue;
        }
        uint32_t bit = 0;
        while ((port_work_[index] & (1U << bit)) == 0)
        {
            ++bit;
        }
        port_work_[index] &= ~(1U << bit);
        port_work_count_.fetch_sub(1);
        *port = index * 32 + bit;
        return true;
    }
    return false;
}

void Controller::OnInterrupt(void* context)
{
    auto* controller = static_cast<Controller*>(context);
    Hardware& hardware = controller->hardware_;
    if ((controller->ReadOperational(Op::UsbSts) & UsbSts::EventInterrupt) != 0)
    {
        controller->WriteOperational(Op::UsbSts, UsbSts::EventInterrupt);
    }
    hardware.Write32(controller->RuntimeOffset(InterrupterRegister(0, Runtime::Iman)),
                     Iman::InterruptPending | Iman::InterruptEnable);
    hardware.SignalEvents();
}

bool Controller::PendingDone(const void* context)
{
    return static_cast<const std::atomic<bool>*>(context)->load(std::memory_order_acquire);
}

void Controller::ProcessEvents()
{
    if (!initialized_ || disabled_.load())
    {
        return;
    }
    Sync::SpinLockGuard guard(event_lock_);
    if (quiesced_.load())
    {
        return;
    }
    const uint32_t status = ReadOperational(Op::UsbSts);
    if ((status & UsbStatusErrors) != 0)
    {
        RaiseError(status);
        return;
    }
    if ((status & UsbSts::EventInterrupt) != 0)
    {
        WriteOperational(Op::UsbSts, UsbSts::EventInterrupt);
    }
    hardware_.Write32(RuntimeOffset(InterrupterRegister(0, Runtime::Iman)),
                      Iman::InterruptPending | Iman::InterruptEnable);
    Trb event;
    bool consumed = false;
    while (events_.Next(&event))
    {
        consumed = true;
        HandleEvent(event);
    }
    if (consumed)
    {
        hardware_.Write64(RuntimeOffset(InterrupterRegister(0, Runtime::Erdp)),
                          events_.DequeuePointer());
    }
}

void Controller::RaiseError(uint32_t status)
{
    if (error_pending_.exchange(true))
    {
        return;
    }
    {
        Sync::SpinLockGuard guard(ring_lock_);
        for (uint32_t index = 0; index < PendingCommands; ++index)
        {
            if (pending_[index].used && !pending_[index].abandoned && !done_[index].load())
            {
                pending_[index].code = CompletionCode::Invalid;
                done_[index].store(true, std::memory_order_release);
            }
        }
    }
    hardware_.WakeWaiters();
    char line[64];
    Text text(line, sizeof(line));
    text.Words("USB: host controller error, USBSTS 0x");
    text.Hex(status, 8);
    if (text.Finish())
    {
        logger.Error(line);
    }
}

void Controller::HandleEvent(const Trb& event)
{
    switch (TrbTypeOf(event))
    {
    case TrbType::CommandCompletionEvent:
        CompleteCommand(event);
        break;
    case TrbType::PortStatusChangeEvent:
        SetPortWork(event.parameter_low >> 24);
        break;
    default:
        break;
    }
}

void Controller::CompleteCommand(const Trb& event)
{
    const uint64_t physical = TrbParameter(event);
    const uint32_t code = event.status >> CompletionShift;
    bool wake = false;
    bool log_unknown = false;
    {
        Sync::SpinLockGuard guard(ring_lock_);
        bool known = false;
        for (uint32_t index = 0; index < PendingCommands; ++index)
        {
            if (!pending_[index].used || pending_[index].trb != physical)
            {
                continue;
            }
            known = true;
            if (pending_[index].abandoned)
            {
                pending_[index].used = false;
            }
            else
            {
                pending_[index].code = code;
                pending_[index].slot =
                    (event.control & TrbControl::SlotMask) >> TrbControl::SlotShift;
                done_[index].store(true, std::memory_order_release);
                wake = true;
            }
            commands_.Retire(physical);
            break;
        }
        if (!known && code != CompletionCode::CommandRingStopped &&
            code != CompletionCode::CommandAborted && !unknown_logged_)
        {
            unknown_logged_ = true;
            log_unknown = true;
        }
    }
    if (wake)
    {
        hardware_.WakeWaiters();
    }
    if (log_unknown)
    {
        logger.Warning("USB: a completion event names no pending command; ignored");
    }
}

void Controller::RingDoorbell(uint32_t index)
{
    hardware_.Write32(doorbells_ + index * 4, 0);
}

CommandResult Controller::Command(const Trb& trb)
{
    Sync::MutexGuard command_guard(command_lock_);
    if (!initialized_ || disabled_.load() || error_pending_.load())
    {
        return {CommandStatus::Unavailable, 0, 0};
    }
    Memory::Dma::ProducerBarrier();
    uint32_t index = PendingCommands;
    uint64_t physical = 0;
    {
        Sync::SpinLockGuard guard(ring_lock_);
        for (uint32_t candidate = 0; candidate < PendingCommands; ++candidate)
        {
            if (!pending_[candidate].used)
            {
                index = candidate;
                break;
            }
        }
        const Trb td[1] = {trb};
        if (index == PendingCommands || !commands_.Enqueue(td, &physical))
        {
            return {CommandStatus::Unavailable, 0, 0};
        }
        pending_[index] = {};
        pending_[index].used = true;
        pending_[index].trb = physical;
        done_[index].store(false);
        Memory::Dma::ProducerBarrier();
        RingDoorbell(0);
    }

    const uint64_t deadline = hardware_.NowMilliseconds() + CommandTimeoutMs;
    if (!hardware_.WaitUntil(PendingDone, &done_[index], deadline))
    {
        bool abandoned = false;
        {
            Sync::SpinLockGuard guard(ring_lock_);
            if (!done_[index].load())
            {
                pending_[index].abandoned = true;
                abandoned = true;
            }
        }
        if (abandoned)
        {
            AbortCommand(index, physical);
            return {CommandStatus::Timeout, 0, 0};
        }
    }
    Sync::SpinLockGuard guard(ring_lock_);
    const CommandResult result = {pending_[index].code == CompletionCode::Success
                                      ? CommandStatus::Success
                                      : CommandStatus::Failed,
                                  pending_[index].code, pending_[index].slot};
    pending_[index].used = false;
    return result;
}

void Controller::AbortCommand(uint32_t index, uint64_t trb_physical)
{
    hardware_.Write64(operational_ + Op::CommandRing, CommandRingControl::CommandAbort);
    if (!WaitFor(operational_ + Op::CommandRing,
                 static_cast<uint32_t>(CommandRingControl::CommandRingRunning), 0, AbortTimeoutMs))
    {
        error_pending_.store(true);
        hardware_.SignalEvents();
        logger.Error("USB: the command ring did not stop after an abort");
        return;
    }
    Sync::SpinLockGuard guard(ring_lock_);
    // The controller resumes after the command it aborted, so no pointer is written back.
    commands_.Retire(trb_physical);
    pending_[index].used = false;
}

bool Controller::AllocateSlot(uint32_t slot, Speed speed, uint32_t port)
{
    Slot& entry = slots_[slot];
    const uint64_t below = ac64_ ? UINT64_MAX : FourGiB;
    if (!hardware_.AllocateDma(layout_.DeviceBytes(), below, &entry.device) ||
        !hardware_.AllocateDma(layout_.InputBytes(), below, &entry.input) ||
        !hardware_.AllocateDma(RingBytes, below, &entry.ep0_buffer) ||
        !entry.ep0.Initialize(entry.ep0_buffer))
    {
        FreeBuffer(hardware_, entry.ep0_buffer);
        FreeBuffer(hardware_, entry.input);
        FreeBuffer(hardware_, entry.device);
        return false;
    }
    entry.in_use = true;
    entry.port = static_cast<uint8_t>(port);
    entry.speed = speed;
    port_slot_[port] = static_cast<uint8_t>(slot);
    Table(dcbaa_)[slot] = entry.device.physical;
    return true;
}

void Controller::ReleaseSlot(uint32_t slot)
{
    Slot& entry = slots_[slot];
    Table(dcbaa_)[slot] = 0;
    if (entry.port != 0 && port_slot_[entry.port] == slot)
    {
        port_slot_[entry.port] = 0;
    }
    FreeBuffer(hardware_, entry.ep0_buffer);
    FreeBuffer(hardware_, entry.input);
    FreeBuffer(hardware_, entry.device);
    entry.in_use = false;
    entry.port = 0;
}

void Controller::DisableSlot(uint32_t slot)
{
    Command(MakeTrb(TrbType::DisableSlot, slot << TrbControl::SlotShift, 0, 0));
}

void Controller::DetachAll()
{
    for (uint32_t slot = 1; slot <= MaxSlots; ++slot)
    {
        if (slots_[slot].in_use)
        {
            ReleaseSlot(slot);
        }
    }
}

bool Controller::AddressDevice(uint32_t port, Speed speed, uint32_t slot)
{
    Slot& entry = slots_[slot];
    const Location location = {speed, static_cast<uint8_t>(port), 0, 0, 0, 0, false};
    const EndpointDescriptor control = {
        0, EndpointType::Control, static_cast<uint16_t>(ControlEndpointPacketSize(speed)), 0, 0, 0,
        0};
    WriteSlot(layout_, entry.input.address, location, 1, nullptr);
    WriteEndpoint(layout_, entry.input.address, control, speed,
                  entry.ep0.Physical() | (entry.ep0.CycleState() ? 1U : 0U));
    const CommandResult result = Command(
        MakeTrb(TrbType::AddressDevice, slot << TrbControl::SlotShift, entry.input.physical, 0));
    return result.status == CommandStatus::Success;
}

bool Controller::ResetPort(uint32_t port)
{
    uint32_t status = ReadPort(port);
    const uint32_t offset = operational_ + PortRegister(port, Op::PortSc);
    if (port_major_[port] >= 3)
    {
        const uint32_t link = (status & PortSc::LinkStateMask) >> PortSc::LinkStateShift;
        if ((status & PortSc::Enabled) != 0 && link == U0)
        {
            return true;
        }
        WritePort(port, PortscWriteValue(status, 0) | PortSc::WarmReset);
        if (!WaitFor(offset, PortSc::WarmResetChange, PortSc::WarmResetChange, PortResetTimeoutMs))
        {
            return false;
        }
    }
    else
    {
        WritePort(port, PortscWriteValue(status, 0) | PortSc::Reset);
        if (!WaitFor(offset, PortSc::ResetChange, PortSc::ResetChange, PortResetTimeoutMs))
        {
            return false;
        }
    }
    status = ReadPort(port);
    WritePort(port, PortscWriteValue(status, status & PortSc::ChangeMask));
    return (ReadPort(port) & PortSc::Enabled) != 0;
}

bool Controller::Enumerate(uint32_t port)
{
    char line[96];
    if (!ResetPort(port))
    {
        if (String::FormatTo(line, sizeof(line), "USB: port {} did not reset", port))
        {
            logger.Warning(line);
        }
        return false;
    }
    const uint32_t status = ReadPort(port);
    Speed speed;
    if (!SpeedFromId(static_cast<uint8_t>((status & PortSc::SpeedMask) >> PortSc::SpeedShift),
                     &speed))
    {
        if (String::FormatTo(line, sizeof(line), "USB: port {} has an unknown speed", port))
        {
            logger.Warning(line);
        }
        return false;
    }
    const CommandResult enabled = Command(
        MakeTrb(TrbType::EnableSlot, static_cast<uint32_t>(port_slot_type_[port]) << 16, 0, 0));
    if (enabled.status != CommandStatus::Success)
    {
        const char* reason = enabled.code == CompletionCode::NoSlotsAvailable
                                 ? "USB: no device slots left for port {}"
                                 : "USB: port {} got no device slot";
        if (String::FormatTo(line, sizeof(line), reason, port))
        {
            logger.Warning(line);
        }
        return false;
    }
    const uint32_t slot = enabled.slot;
    if (slot == 0 || slot > max_slots_ || slots_[slot].in_use || !AllocateSlot(slot, speed, port))
    {
        if (slot != 0 && slot <= max_slots_ && !slots_[slot].in_use)
        {
            DisableSlot(slot);
        }
        if (String::FormatTo(line, sizeof(line), "USB: port {} could not set up its device slot",
                             port))
        {
            logger.Warning(line);
        }
        return false;
    }
    if (!AddressDevice(port, speed, slot))
    {
        DisableSlot(slot);
        ReleaseSlot(slot);
        if (String::FormatTo(line, sizeof(line), "USB: port {} could not be addressed", port))
        {
            logger.Warning(line);
        }
        return false;
    }
    if (String::FormatTo(line, sizeof(line), "USB: port {} {} device addressed as slot {}", port,
                         SpeedName(speed), slot))
    {
        logger.Info(line);
    }
    return true;
}

void Controller::HandlePort(uint32_t port)
{
    uint32_t status = ReadPort(port);
    if ((status & PortSc::ChangeMask) != 0)
    {
        WritePort(port, PortscWriteValue(status, status & PortSc::ChangeMask));
    }
    const bool connected = (status & PortSc::CurrentConnect) != 0;
    const bool changed = (status & PortSc::ConnectChange) != 0;
    const uint32_t slot = port_slot_[port];
    if (changed)
    {
        port_failed_[port] = false;
    }
    if (!connected || (changed && slot != 0))
    {
        if (slot != 0)
        {
            DisableSlot(slot);
            ReleaseSlot(slot);
            char line[48];
            if (String::FormatTo(line, sizeof(line), "USB: port {} device removed", port))
            {
                logger.Info(line);
            }
        }
        if (!connected)
        {
            return;
        }
    }
    else if (slot != 0 || port_failed_[port])
    {
        return;
    }
    hardware_.Pause(DebounceMs);
    if ((ReadPort(port) & PortSc::CurrentConnect) == 0)
    {
        return;
    }
    port_failed_[port] = !Enumerate(port);
}

bool Controller::HasManagerWork() const
{
    return !disabled_.load() && (error_pending_.load() || port_work_count_.load() != 0);
}

void Controller::RunManagerWork()
{
    if (!initialized_ || disabled_.load())
    {
        return;
    }
    if (error_pending_.load())
    {
        Restart();
    }
    uint32_t port;
    while (!disabled_.load() && !error_pending_.load() && TakePortWork(&port))
    {
        HandlePort(port);
    }
}

void Controller::Restart()
{
    quiesced_.store(true);
    {
        Sync::SpinLockGuard drained(event_lock_);
    }
    DetachAll();
    for (uint32_t port = 0; port <= MaxPorts; ++port)
    {
        port_failed_[port] = false;
    }
    const bool allowed = restarts_ == 0;
    ++restarts_;
    const bool restarted = allowed && Halt() && Reset() && Program();
    if (!restarted)
    {
        disabled_.store(true);
        logger.Error("USB: the controller is disabled after a second error");
        WriteOperational(Op::UsbCmd, 0);
    }
    error_pending_.store(false);
    quiesced_.store(false);
    if (restarted)
    {
        logger.Warning("USB: the controller was reset after an error");
        QueueConnectedPorts();
    }
}

bool Controller::Describe(char* buffer, size_t capacity) const
{
    Text text(buffer, capacity);
    text.Words("USB: xHCI ");
    text.Hex(function_.vendor, 4);
    text.Put(':');
    text.Hex(function_.device, 4);
    text.Words(" at ");
    AppendAddress(text, function_);
    text.Words(", ");
    text.Decimal(max_ports_);
    text.Words(" ports, ");
    text.Decimal(max_slots_);
    text.Words(" slots, ");
    text.Decimal(layout_.ContextSize());
    text.Words("-byte contexts, ");
    text.Words(InterruptName(interrupts_));
    return text.Finish();
}

uint32_t Controller::RootPorts() const
{
    return max_ports_;
}

uint32_t Controller::SlotCount() const
{
    return max_slots_;
}

InterruptKind Controller::Interrupts() const
{
    return interrupts_;
}

bool Controller::InterruptRegistered() const
{
    return interrupts_chosen_ && interrupts_ != InterruptKind::Polling;
}

bool Controller::Disabled() const
{
    return disabled_.load();
}

uint32_t Controller::Restarts() const
{
    return restarts_;
}

bool Controller::GetSlot(uint32_t slot, SlotInfo* info) const
{
    if (slot == 0 || slot > MaxSlots || !slots_[slot].in_use)
    {
        return false;
    }
    *info = {true, slots_[slot].port, slots_[slot].speed};
    return true;
}

} // namespace FeatherOS::Usb::Xhci
