#ifndef FEATHEROS_USB_XHCI_HARDWARE_H
#define FEATHEROS_USB_XHCI_HARDWARE_H

#include "kernel/memory/dma.h"

#include <stdint.h>

namespace FeatherOS::Usb::Xhci
{

enum class InterruptKind : uint8_t
{
    Polling,
    MsiX,
    Msi,
    Intx,
};

// Everything the controller logic needs from the machine, so the same code runs on the host
// against a simulated controller. Register offsets count from the start of BAR 0.
class Hardware
{
public:
    virtual uint32_t Read32(uint32_t offset) = 0;
    virtual void Write32(uint32_t offset, uint32_t value) = 0;
    // A 64-bit register as two dword writes, low first, as controllers without 64-bit access need.
    virtual void Write64(uint32_t offset, uint64_t value) = 0;

    virtual uint32_t ReadConfig32(uint16_t offset) = 0;
    virtual void WriteConfig32(uint16_t offset, uint32_t value) = 0;

    // below is an exclusive physical limit.
    virtual bool AllocateDma(uint64_t bytes, uint64_t below, Memory::Dma::Buffer* buffer) = 0;
    virtual void FreeDma(const Memory::Dma::Buffer& buffer) = 0;

    virtual uint64_t NowMilliseconds() = 0;
    // Gives other tasks the CPU for about this long, which is also how a polling wait lets the
    // event task run.
    virtual void Pause(uint32_t milliseconds) = 0;
    // Blocks until done(context) is true or the deadline passes; returns done's last answer.
    // WakeWaiters ends the sleep early.
    virtual bool WaitUntil(bool (*done)(const void* context), const void* context,
                           uint64_t deadline_milliseconds) = 0;
    virtual void WakeWaiters() = 0;
    // Called by the interrupt top half and by anything that wants the event task to run.
    virtual void SignalEvents() = 0;
    // The event task sleeps here until SignalEvents or the deadline.
    virtual void WaitForEvents(uint64_t deadline_milliseconds) = 0;

    // handler(context) runs on an interrupt of the function. Polling when allow is false or
    // nothing else is available.
    virtual InterruptKind SetUpInterrupts(bool allow, void (*handler)(void* context),
                                          void* context) = 0;

protected:
    Hardware() = default;
    ~Hardware() = default;
    Hardware(const Hardware&) = default;
    Hardware& operator=(const Hardware&) = default;
};

} // namespace FeatherOS::Usb::Xhci

#endif
