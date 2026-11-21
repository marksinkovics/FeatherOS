#ifndef FEATHEROS_TESTS_USB_FAKE_XHCI_H
#define FEATHEROS_TESTS_USB_FAKE_XHCI_H

#include "kernel/memory/dma.h"
#include "kernel/usb/host_controller.h"
#include "kernel/usb/xhci/trb.h"

#include <cstdint>
#include <map>
#include <vector>

namespace FeatherOS::Usb::Testing
{

// Host memory that stands for physical memory: the ring code writes through the buffer's address
// and the fake controller reads the same bytes through the physical address.
class PhysicalMemory
{
public:
    PhysicalMemory() = default;
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
    uint64_t next_physical_ = 0x1'2000'0000ULL;
};

// What it models: the capability, operational, port and interrupter 0 registers; the command
// ring, transfer rings and event ring read as hardware reads them (cycle bits, Link TRBs with
// Toggle Cycle, one event segment, no overwriting of unconsumed events); PORTSC write
// semantics. Not yet: device contexts, scripted devices and hubs, data stages, injected errors.
class FakeXhci
{
public:
    static constexpr uint32_t CapabilityLength = 0x40;
    static constexpr uint32_t DoorbellBase = 0x2000;
    static constexpr uint32_t RuntimeBase = 0x1000;
    static constexpr uint32_t MaxSlots = 64;

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
    void Connect(uint32_t port, Speed speed);
    void Disconnect(uint32_t port);

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
    uint32_t next_slot_ = 1;
    std::map<uint32_t, Endpoint> endpoints_;
    std::vector<uint32_t> command_types_;
    std::vector<uint32_t> transfer_types_;
    std::vector<uint64_t> erdp_writes_;
    uint32_t dropped_events_ = 0;
    uint32_t faults_ = 0;
};

} // namespace FeatherOS::Usb::Testing

#endif
