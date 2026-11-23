#ifndef FEATHEROS_STORAGE_CONTROLLER_HARDWARE_H
#define FEATHEROS_STORAGE_CONTROLLER_HARDWARE_H

#include "kernel/memory/dma.h"

#include <stdint.h>

namespace FeatherOS::Storage
{

enum class InterruptKind : uint8_t
{
    Polling,
    MsiX,
    Msi,
};

// Register offsets count from the start of the register BAR, so the same controller code runs on
// the host against a simulated controller.
class ControllerHardware
{
public:
    virtual uint32_t Read32(uint32_t offset) = 0;
    virtual void Write32(uint32_t offset, uint32_t value) = 0;
    // Two dword accesses, low first.
    virtual uint64_t Read64(uint32_t offset) = 0;
    virtual void Write64(uint32_t offset, uint64_t value) = 0;

    virtual uint32_t ReadConfig32(uint16_t offset) = 0;

    // below is an exclusive physical limit.
    virtual bool AllocateDma(uint64_t bytes, uint64_t below, Memory::Dma::Buffer* buffer) = 0;
    virtual void FreeDma(const Memory::Dma::Buffer& buffer) = 0;

    // handler(context) runs on an interrupt of the function and must only wake waiters.
    virtual InterruptKind SetUpInterrupts(void (*handler)(void* context), void* context) = 0;

protected:
    ControllerHardware() = default;
    ~ControllerHardware() = default;
    ControllerHardware(const ControllerHardware&) = default;
    ControllerHardware& operator=(const ControllerHardware&) = default;
};

} // namespace FeatherOS::Storage

#endif
