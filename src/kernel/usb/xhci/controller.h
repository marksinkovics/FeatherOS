#ifndef FEATHEROS_USB_XHCI_CONTROLLER_H
#define FEATHEROS_USB_XHCI_CONTROLLER_H

#include "kernel/pci/pci.h"
#include "kernel/sync/mutex.h"
#include "kernel/sync/spinlock.h"
#include "kernel/usb/host_controller.h"
#include "kernel/usb/xhci/context.h"
#include "kernel/usb/xhci/hardware.h"
#include "kernel/usb/xhci/ring.h"
#include "kernel/usb/xhci/trb.h"
#include "user/thread_safety.h"

#include <atomic>
#include <stddef.h>
#include <stdint.h>

namespace FeatherOS::Usb::Xhci
{

constexpr uint32_t MaxPorts = MaxRootPorts;
constexpr uint32_t MaxSlots = MaxDeviceSlots;
constexpr uint32_t PendingCommands = 4;
constexpr uint32_t PendingTransfers = 128;
constexpr uint32_t ContextIndexes = 32;
constexpr uint32_t MaxTransferBytes = 16384;
constexpr uint32_t MaxTdTrbs = 3;
constexpr uint32_t TransactionRetries = 3;
constexpr uint32_t CompletionBatch = 16;
constexpr uint32_t ControlTimeoutMs = 5000;

enum class CommandStatus : uint8_t
{
    Success,
    Failed,
    Timeout,
    Unavailable,
};

struct CommandResult
{
    CommandStatus status;
    uint32_t code;
    uint32_t slot;
};

struct SlotInfo
{
    bool in_use;
    uint8_t port;
    Speed speed;
};

// One controller. Three kinds of task use it: the event task (ProcessEvents), the manager task
// (everything that blocks) and, through the interrupt top half, any CPU.
class Controller final : public HostController
{
public:
    // Maps the function's registers and brings the controller up; null when it cannot.
    static Controller* Probe(Pci::ConfigSpace& config, const Pci::Function& function);

    Controller(Hardware& hardware, const Pci::Function& function);
    ~Controller();
    Controller(const Controller&) = delete;
    Controller& operator=(const Controller&) = delete;

    bool Initialize(bool allow_interrupts);

    // The event task: drains the event ring, completes commands and transfers, and runs the
    // completion callbacks with no lock held.
    void ProcessEvents();

    bool Describe(char* buffer, size_t capacity) const;
    uint32_t SlotCount() const;
    InterruptKind Interrupts() const;
    // A registered handler keeps a pointer to the controller, which then must never be freed.
    bool InterruptRegistered() const;
    bool Disabled() const;
    uint32_t Restarts() const;
    bool GetSlot(uint32_t slot, SlotInfo* info) const;
    Hardware& Machine();
    uint32_t RecentRecoveries(uint32_t slot, uint8_t endpoint);

    uint32_t RootPorts() const override;
    void ReadRootPort(uint32_t port, RootPortStatus* status, bool acknowledge) override;
    bool ResetRootPort(uint32_t port, Speed* speed, bool force) override;
    CreateStatus CreateDevice(const Location& location, uint32_t* slot) override;
    bool SetControlMaxPacket(uint32_t slot, uint32_t max_packet) override;
    bool ConfigureEndpoints(uint32_t slot, std::span<const EndpointDescriptor> endpoints,
                            const HubInfo* hub) override;
    void DestroyDevice(uint32_t slot) override;
    Status Control(uint32_t slot, const SetupPacket& setup, Memory::Dma::Buffer& buffer,
                   uint32_t* actual) override;
    bool Submit(uint32_t slot, Transfer& transfer) override;
    bool Wait(Transfer& transfer, uint64_t deadline_milliseconds) override;
    bool ResetEndpoint(uint32_t slot, uint8_t endpoint) override;
    bool CancelEndpoint(uint32_t slot, uint8_t endpoint) override;
    bool ClearHalt(uint32_t slot, uint8_t endpoint) override;
    void StopEndpoints(uint32_t slot) override;
    bool AllocateBuffer(uint64_t bytes, Memory::Dma::Buffer* buffer) override;
    void FreeBuffer(const Memory::Dma::Buffer& buffer) override;
    void Delay(uint32_t milliseconds) override;
    uint64_t NowMilliseconds() override;
    bool HasWork() const override;
    bool TakePortChange(uint32_t* port) override;
    bool Recover() override;
    void ServiceEndpoints() override;
    void SetRecoveryHandler(RecoveryHandler handler, void* context) override;

private:
    struct Pending
    {
        bool used = false;
        bool abandoned = false;
        uint64_t trb = 0;
        uint32_t code = 0;
        uint32_t slot = 0;
    };

    enum class TransferState : uint8_t
    {
        Free,
        Active,
        Failed,
    };

    // One TD in flight. Events name any TRB of it; the last one ends the transfer.
    struct PendingTransfer
    {
        TransferState state = TransferState::Free;
        bool control = false;
        bool short_seen = false;
        uint8_t slot = 0;
        uint8_t dci = 0;
        uint8_t attempts = 0;
        uint8_t trbs = 0;
        Transfer* transfer = nullptr;
        uint64_t first = 0;
        uint64_t last = 0;
        uint32_t first_chunk = 0;
        uint32_t actual = 0;
        uint32_t code = 0;
        SetupPacket setup{};
    };

    struct EndpointRing
    {
        ProducerRing ring;
        bool active = false;
        bool halted = false;
        bool quiescing = false;
        bool recovery_needed = false;
        uint8_t address = 0;
        EndpointDescriptor descriptor{};
        uint32_t recoveries = 0;
        uint64_t recovery_window = 0;
    };

    struct Slot
    {
        bool in_use = false;
        uint8_t port = 0;
        Speed speed = Speed::Full;
        uint32_t entries = 1;
        Location location{};
        Memory::Dma::Buffer device{};
        Memory::Dma::Buffer input{};
        Memory::Dma::Buffer rings[ContextIndexes]{};
    };

    struct Completions
    {
        Transfer* items[CompletionBatch];
        uint32_t count = 0;
    };

    static void OnInterrupt(void* context);
    static bool PendingDone(const void* context);

    uint32_t ReadOperational(uint32_t reg);
    void WriteOperational(uint32_t reg, uint32_t value);
    uint32_t ReadPort(uint32_t port);
    void WritePort(uint32_t port, uint32_t value);
    uint32_t RuntimeOffset(uint32_t reg) const;
    bool WaitFor(uint32_t offset, uint32_t mask, uint32_t value, uint32_t timeout_milliseconds);

    bool Setup();
    void RouteIntelPorts();
    void HandOff();
    void WalkCapabilities();
    bool Halt();
    bool Reset();
    bool Program();
    bool AllocateTables();
    void FreeTables();
    void ReadProtocols();
    void QueueConnectedPorts();
    void SetPortWork(uint32_t port);

    bool DrainEvents(Completions& completions);
    void RaiseError(uint32_t status);
    void HandleEvent(const Trb& event, Completions& completions);
    void CompleteCommand(const Trb& event);
    void HandleTransferEvent(const Trb& event, Completions& completions);
    void RingDoorbell(uint32_t index, uint32_t target = 0);

    CommandResult Command(const Trb& trb);
    void AbortCommand(uint32_t index, uint64_t trb_physical);

    bool ResetPort(uint32_t port, bool force);
    bool AddressDevice(uint32_t slot);
    bool AllocateSlot(uint32_t slot, const Location& location);
    void ReleaseSlot(uint32_t slot);
    void DisableSlot(uint32_t slot);
    void ClearInput(uint32_t slot);
    uint64_t RingPointer(uint32_t slot, uint32_t dci);
    void DetachAll();
    void Restart();

    bool SubmitTransfer(uint32_t slot, Transfer& transfer, const SetupPacket* setup);
    bool QueueTransferLocked(PendingTransfer& entry) REQUIRES(ring_lock_);
    PendingTransfer* FindTransferLocked(uint32_t slot, uint32_t dci, uint64_t pointer)
        REQUIRES(ring_lock_);
    PendingTransfer* FindFailedLocked(uint32_t slot, uint32_t dci) REQUIRES(ring_lock_);
    PendingTransfer* FindWaitingLocked(const Transfer& transfer) REQUIRES(ring_lock_);
    bool AwaitTransfer(Transfer& transfer, uint32_t dci, uint64_t deadline_milliseconds);
    void FinishTransfer(Transfer& transfer);
    void FinishTransfer(Transfer& transfer, Status status);
    void AbortTransfers(uint32_t slot, uint32_t dci_mask, Status status);
    void RecoverEndpoint(uint32_t slot, uint32_t dci);
    void Quiesce(uint32_t slot, uint32_t dci, Transfer* waited, Status waited_status, bool resume);
    bool DeviceGone(uint32_t slot);
    bool ClearEndpointFeature(uint32_t slot, uint8_t address);
    bool ResetSequence(uint32_t slot, uint32_t dci);
    uint32_t RecordRecoveryLocked(uint32_t slot, uint32_t dci) REQUIRES(ring_lock_);

    Hardware& hardware_;
    Pci::Function function_;
    ContextLayout layout_;
    InterruptKind interrupts_ = InterruptKind::Polling;
    bool allow_interrupts_ = true;
    bool initialized_ = false;
    bool interrupts_chosen_ = false;
    bool intel_ = false;
    bool ac64_ = false;
    bool power_control_ = false;
    uint32_t operational_ = 0;
    uint32_t runtime_ = 0;
    uint32_t doorbells_ = 0;
    uint32_t legacy_ = 0;
    uint32_t extended_ = 0;
    uint32_t max_slots_ = 0;
    uint32_t max_ports_ = 0;
    uint32_t scratchpads_ = 0;
    uint8_t port_major_[MaxPorts + 1] = {};
    uint8_t port_slot_type_[MaxPorts + 1] = {};
    Slot slots_[MaxSlots + 1];

    Memory::Dma::Buffer dcbaa_{};
    Memory::Dma::Buffer scratchpad_array_{};
    Memory::Dma::Buffer scratchpad_pages_{};
    Memory::Dma::Buffer command_buffer_{};
    Memory::Dma::Buffer event_buffer_{};
    Memory::Dma::Buffer segment_table_{};
    // Read only by ProcessEvents, and set up by Program while the event task is quiesced.
    EventRing events_;
    Sync::SpinLock event_lock_;
    std::atomic<bool> quiesced_ = false;

    Sync::Mutex command_lock_;
    Sync::SpinLock ring_lock_;
    ProducerRing commands_ GUARDED_BY(ring_lock_);
    Pending pending_[PendingCommands] GUARDED_BY(ring_lock_);
    std::atomic<bool> done_[PendingCommands] = {};
    bool unknown_logged_ GUARDED_BY(ring_lock_) = false;
    EndpointRing endpoint_rings_[MaxSlots + 1][ContextIndexes] GUARDED_BY(ring_lock_);
    PendingTransfer transfers_[PendingTransfers] GUARDED_BY(ring_lock_);
    bool unknown_transfer_logged_ GUARDED_BY(ring_lock_) = false;
    // One bit per device context index with an error the manager has yet to recover.
    std::atomic<uint32_t> recovery_mask_[MaxSlots + 1] = {};
    std::atomic<uint32_t> recovery_count_ = 0;
    RecoveryHandler recovery_handler_ = nullptr;
    void* recovery_context_ = nullptr;

    Sync::SpinLock work_lock_;
    uint32_t port_work_[(MaxPorts + 32) / 32] GUARDED_BY(work_lock_) = {};
    std::atomic<uint32_t> port_work_count_ = 0;
    std::atomic<bool> error_pending_ = false;
    std::atomic<bool> disabled_ = false;
    uint32_t restarts_ = 0;
};

} // namespace FeatherOS::Usb::Xhci

#endif
