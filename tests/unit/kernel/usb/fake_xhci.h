#ifndef FEATHEROS_TESTS_USB_FAKE_XHCI_H
#define FEATHEROS_TESTS_USB_FAKE_XHCI_H

#include "kernel/memory/dma.h"
#include "kernel/usb/host_controller.h"
#include "kernel/usb/xhci/trb.h"

#include <cstdint>
#include <map>
#include <set>
#include <vector>

namespace FeatherOS::Usb::Testing
{

// Host memory that stands for physical memory: the ring code writes through the buffer's address
// and the fake controller reads the same bytes through the physical address.
class PhysicalMemory
{
public:
    explicit PhysicalMemory(uint64_t base = 0x1'2000'0000ULL);
    PhysicalMemory(const PhysicalMemory&) = delete;
    PhysicalMemory& operator=(const PhysicalMemory&) = delete;
    ~PhysicalMemory();

    Memory::Dma::Buffer Allocate(uint64_t bytes);
    volatile uint8_t* Translate(uint64_t physical, uint64_t length) const;

private:
    struct Region
    {
        uint64_t physical;
        uint8_t* host;
        uint64_t size;
    };

    std::vector<Region> regions_;
    uint64_t next_physical_;
};

// What it models: the capability, operational, port and interrupter 0 registers; the command
// ring, transfer rings and event ring read as hardware reads them (cycle bits, Link TRBs with
// Toggle Cycle, one event segment, no overwriting of unconsumed events); PORTSC write
// semantics; the USB Legacy Support and Supported Protocol capabilities, a firmware that gives up
// ownership after a set number of polls, reset and halt delays with the Controller Not Ready flag,
// scratchpad and page-size settings, Enable Slot, Disable Slot, Address Device (reads the input
// context and writes the device context's slot and EP0), a command that never completes until the
// command ring is aborted, and injected events and errors. Not yet: scripted devices and hubs,
// data stages.
class FakeXhci
{
public:
    static constexpr uint32_t CapabilityLength = 0x40;
    static constexpr uint32_t DoorbellBase = 0x2000;
    static constexpr uint32_t RuntimeBase = 0x1000;
    static constexpr uint32_t MaxSlots = 64;
    static constexpr uint32_t ExtendedBase = 0x3000;

    FakeXhci(PhysicalMemory& memory, uint32_t ports, bool context64);

    static constexpr uint32_t OperationalOffset(uint32_t reg)
    {
        return CapabilityLength + reg;
    }
    static constexpr uint32_t RuntimeOffset(uint32_t reg)
    {
        return RuntimeBase + reg;
    }
    static constexpr uint32_t DoorbellOffset(uint32_t index)
    {
        return DoorbellBase + index * 4;
    }

    uint32_t Read32(uint32_t offset);
    // Writes the high dword first, so the low dword's side effects see the whole value.
    void Write32(uint32_t offset, uint32_t value);
    void Write64(uint32_t offset, uint64_t value);

    void SetEndpointRing(uint32_t slot, uint32_t dci, uint64_t ring, bool cycle);
    // A SuperSpeed device on a USB 3 port is enabled in U0 unless it is not trained.
    void Connect(uint32_t port, Speed speed, bool trained = true);
    void Disconnect(uint32_t port);

    void EnableLegacySupport(bool firmware_owns, int release_after_polls);
    // Ports from usb3_first up are USB 3; 0 makes every port USB 2.
    void SetUsb3Ports(uint32_t usb3_first);
    void SetScratchpads(uint32_t count);
    void SetAc64(bool ac64);
    void SetPageSize(uint32_t page_size_bits);
    void SetPortPowerControl(bool enabled);
    void StartRunning();
    // After HCRST, USBSTS shows Controller Not Ready for this many reads of it; never ready if
    // never_ready.
    void SetResetDelay(uint32_t polls, bool never_ready);
    void SetHaltDelay(uint32_t polls, bool never_halts);
    // The next command of this type is taken off the ring and never completes, until a command
    // abort.
    void HoldNextCommand(uint32_t type);
    bool PostInjectedEvent(const Xhci::Trb& event);
    void SetStatusBits(uint32_t bits);
    // Completes the held command as if it had just run.
    void ReleaseHeldCommand();
    uint64_t LastHeldTrb() const;
    // Enable Slot answers No Slots Available once this many slots are enabled.
    void SetSlotLimit(uint32_t limit);
    uint32_t PortResets(uint32_t port) const;

    struct ResetRecord
    {
        bool halted;
        bool legacy_present;
        bool os_owned;
        bool bios_owned;
        uint32_t smi_enables;
        uint32_t smi_status;
    };

    struct AddressedDevice
    {
        uint32_t slot;
        uint32_t speed_id;
        uint32_t root_port;
        uint32_t route;
        uint32_t entries;
        uint32_t ep0_max_packet;
        uint64_t ep0_ring;
        uint32_t add_flags;
    };

    const std::vector<ResetRecord>& Resets() const;
    const std::vector<AddressedDevice>& Addressed() const;
    const std::vector<uint32_t>& DisabledSlots() const;
    uint32_t NotReadyAccesses() const;
    uint32_t AbortCount() const;
    uint32_t SmiEnables() const;
    uint32_t SmiStatus() const;
    bool OsOwned() const;
    bool BiosOwned() const;
    uint64_t DcbaaEntry(uint32_t index);
    uint64_t DcbaaPointer() const;
    uint32_t UsbCommand() const;
    uint32_t ConfiguredSlots() const;
    uint32_t InterruptModeration() const;
    uint32_t ErstSize() const;
    const std::vector<uint32_t>& CommandTypes() const;
    const std::vector<uint32_t>& TransferTypes() const;
    const std::vector<uint64_t>& EventPointerWrites() const;
    uint32_t DroppedEvents() const;
    uint32_t Faults() const;
    uint32_t PortEnableClears(uint32_t port) const;
    uint32_t PortStatus(uint32_t port) const;

private:
    struct Endpoint
    {
        uint64_t dequeue;
        bool cycle;
    };

    void ProcessCommands();
    void ProcessTransfers(uint32_t slot, uint32_t dci);
    bool ReadTrb(uint64_t physical, Xhci::Trb* trb);
    bool PostEvent(const Xhci::Trb& event);
    void PostPortEvent(uint32_t port);
    void WritePortRegister(uint32_t port, uint32_t value);
    void WriteOperationalRegister(uint32_t reg, uint32_t value);
    void WriteRuntimeRegister(uint32_t reg, uint32_t value);
    uint32_t ReadRuntimeRegister(uint32_t reg) const;
    void Reset();
    uint32_t ReadExtended(uint32_t offset);
    void WriteExtended(uint32_t offset, uint32_t value);
    void BuildProtocols();
    bool PortIsUsb3(uint32_t port) const;
    void CountNotReadyAccess(uint32_t offset);
    uint32_t ReadStatus();
    void AbortCommands();
    void Execute(const Xhci::Trb& trb, uint64_t physical);
    void AddressDevice(uint32_t slot, uint64_t input_physical, uint32_t* code);

    PhysicalMemory& memory_;
    uint32_t ports_;
    bool context64_;
    uint32_t usbcmd_ = 0;
    uint32_t usbsts_ = 0;
    uint32_t config_ = 0;
    uint64_t dcbaap_ = 0;
    uint64_t command_dequeue_ = 0;
    bool command_cycle_ = true;
    std::vector<uint32_t> portsc_;
    std::vector<uint32_t> enable_clears_;
    uint32_t iman_ = 0;
    uint32_t imod_ = 0;
    uint32_t erst_size_ = 0;
    uint64_t erst_base_ = 0;
    uint64_t segment_base_ = 0;
    uint32_t segment_size_ = 0;
    uint64_t erdp_ = 0;
    uint32_t event_enqueue_ = 0;
    bool event_cycle_ = true;
    bool event_busy_ = false;
    std::map<uint32_t, Endpoint> endpoints_;
    std::set<uint32_t> enabled_slots_;
    std::vector<uint32_t> protocol_words_;
    bool legacy_present_ = false;
    bool bios_owned_ = false;
    bool os_owned_ = false;
    int release_after_ = -1;
    int legacy_polls_ = 0;
    uint32_t smi_enables_ = 0;
    uint32_t smi_status_ = 0;
    uint32_t usb3_first_ = 0;
    uint32_t scratchpads_ = 0;
    bool ac64_ = true;
    uint32_t page_size_bits_ = 1;
    bool port_power_control_ = false;
    bool reset_pending_ = false;
    uint32_t reset_delay_ = 0;
    uint32_t ready_countdown_ = 0;
    bool never_ready_ = false;
    uint32_t halt_delay_ = 0;
    uint32_t halt_countdown_ = 0;
    bool never_halts_ = false;
    bool halt_pending_ = false;
    uint32_t hold_type_ = 0;
    bool held_ = false;
    uint64_t held_trb_ = 0;
    uint64_t last_held_trb_ = 0;
    Xhci::Trb held_command_ = {};
    uint32_t slot_limit_ = MaxSlots;
    std::vector<uint32_t> port_resets_;
    uint32_t abort_count_ = 0;
    uint32_t not_ready_accesses_ = 0;
    std::vector<ResetRecord> resets_;
    std::vector<AddressedDevice> addressed_;
    std::vector<uint32_t> disabled_slots_;
    std::vector<uint32_t> command_types_;
    std::vector<uint32_t> transfer_types_;
    std::vector<uint64_t> erdp_writes_;
    uint32_t dropped_events_ = 0;
    uint32_t faults_ = 0;
};

} // namespace FeatherOS::Usb::Testing

#endif
