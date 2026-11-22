#ifndef FEATHEROS_TESTS_USB_FAKE_HARDWARE_H
#define FEATHEROS_TESTS_USB_FAKE_HARDWARE_H

#include "fake_xhci.h"

#include "kernel/usb/xhci/hardware.h"

#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <utility>
#include <vector>

namespace FeatherOS::Usb::Testing
{

// The controller's machine over FakeXhci: the clock moves only when the controller pauses or
// waits, and each step of it runs on_idle, which stands for the event task.
class FakeHardware final : public Xhci::Hardware
{
public:
    explicit FakeHardware(FakeXhci& controller, PhysicalMemory& memory);

    uint32_t Read32(uint32_t offset) override;
    void Write32(uint32_t offset, uint32_t value) override;
    void Write64(uint32_t offset, uint64_t value) override;
    uint32_t ReadConfig32(uint16_t offset) override;
    void WriteConfig32(uint16_t offset, uint32_t value) override;
    bool AllocateDma(uint64_t bytes, uint64_t below, Memory::Dma::Buffer* buffer) override;
    void FreeDma(const Memory::Dma::Buffer& buffer) override;
    uint64_t NowMilliseconds() override;
    void Pause(uint32_t milliseconds) override;
    bool WaitUntil(bool (*done)(const void* context), const void* context,
                   uint64_t deadline_milliseconds) override;
    void WakeWaiters() override;
    void SignalEvents() override;
    void WaitForEvents(uint64_t deadline_milliseconds) override;
    Xhci::InterruptKind SetUpInterrupts(bool allow, void (*handler)(void* context),
                                        void* context) override;

    struct ConfigWrite
    {
        uint16_t offset;
        uint32_t value;
    };

    std::function<void()> on_idle;
    // Real time each Pause also takes, for tests where another thread has to make progress.
    uint32_t pause_microseconds = 0;
    Xhci::InterruptKind available = Xhci::InterruptKind::MsiX;
    uint64_t allocation_limit = ~0ULL;
    uint32_t allocations_before_failure = ~0U;

    // With deferral on, doorbell writes from any thread are queued; PumpDoorbells, called by
    // the one thread that stands for the controller, applies them in order.
    void DeferDoorbells(bool defer);
    bool PumpDoorbells();

    void FireInterrupt();
    uint64_t Now() const;
    uint32_t Outstanding() const;
    uint32_t Wakes() const;
    uint32_t Signals() const;
    bool InterruptsAllowed() const;
    bool InterruptsRequested() const;
    const std::vector<ConfigWrite>& ConfigWrites() const;
    void SetConfig(uint16_t offset, uint32_t value);
    const std::vector<Memory::Dma::Buffer>& Live() const;

private:
    FakeXhci& controller_;
    PhysicalMemory& memory_;
    uint64_t now_ = 0;
    std::map<uint16_t, uint32_t> config_;
    std::vector<ConfigWrite> config_writes_;
    std::vector<Memory::Dma::Buffer> live_;
    std::mutex doorbell_mutex_;
    std::vector<std::pair<uint32_t, uint32_t>> doorbells_;
    bool defer_doorbells_ = false;
    uint32_t wakes_ = 0;
    uint32_t signals_ = 0;
    bool allowed_ = false;
    bool requested_ = false;
    void (*handler_)(void*) = nullptr;
    void* handler_context_ = nullptr;
};

} // namespace FeatherOS::Usb::Testing

#endif
