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

constexpr uint32_t MaxPorts = 255;
constexpr uint32_t MaxSlots = 64;
constexpr uint32_t PendingCommands = 4;

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
// (RunManagerWork and everything it calls) and, through the interrupt top half, any CPU.
class Controller
{
public:
    // Maps the function's registers and brings the controller up; null when it cannot.
    static Controller* Probe(Pci::ConfigSpace& config, const Pci::Function& function);

    Controller(Hardware& hardware, const Pci::Function& function);
    ~Controller();
    Controller(const Controller&) = delete;
    Controller& operator=(const Controller&) = delete;

    bool Initialize(bool allow_interrupts);

    // The event task: drains the event ring and completes commands.
    void ProcessEvents();
    bool HasManagerWork() const;
    // The manager task: restarts after a controller error, then handles each port with news.
    void RunManagerWork();

    bool Describe(char* buffer, size_t capacity) const;
    uint32_t RootPorts() const;
    uint32_t SlotCount() const;
    InterruptKind Interrupts() const;
    // A registered handler keeps a pointer to the controller, which then must never be freed.
    bool InterruptRegistered() const;
    bool Disabled() const;
    uint32_t Restarts() const;
    bool GetSlot(uint32_t slot, SlotInfo* info) const;
    Hardware& Machine();

private:
    struct Pending
    {
        bool used = false;
        bool abandoned = false;
        uint64_t trb = 0;
        uint32_t code = 0;
        uint32_t slot = 0;
    };

    struct Slot
    {
        bool in_use = false;
        uint8_t port = 0;
        Speed speed = Speed::Full;
        Memory::Dma::Buffer device{};
        Memory::Dma::Buffer input{};
        Memory::Dma::Buffer ep0_buffer{};
        ProducerRing ep0;
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
    bool TakePortWork(uint32_t* port);

    void RaiseError(uint32_t status);
    void HandleEvent(const Trb& event);
    void CompleteCommand(const Trb& event);
    void RingDoorbell(uint32_t index);

    CommandResult Command(const Trb& trb);
    void AbortCommand(uint32_t index, uint64_t trb_physical);

    void HandlePort(uint32_t port);
    bool Enumerate(uint32_t port);
    bool ResetPort(uint32_t port);
    bool AddressDevice(uint32_t port, Speed speed, uint32_t slot);
    bool AllocateSlot(uint32_t slot, Speed speed, uint32_t port);
    void ReleaseSlot(uint32_t slot);
    void DisableSlot(uint32_t slot);
    void DetachAll();
    void Restart();

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
    uint8_t port_slot_[MaxPorts + 1] = {};
    bool port_failed_[MaxPorts + 1] = {};
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

    Sync::SpinLock work_lock_;
    uint32_t port_work_[(MaxPorts + 32) / 32] GUARDED_BY(work_lock_) = {};
    std::atomic<uint32_t> port_work_count_ = 0;
    std::atomic<bool> error_pending_ = false;
    std::atomic<bool> disabled_ = false;
    uint32_t restarts_ = 0;
};

} // namespace FeatherOS::Usb::Xhci

#endif
