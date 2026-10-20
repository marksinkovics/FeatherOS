#include "kernel/arch/rtc.h"

#include "kernel/memory/device_memory.h"

#include <stddef.h>

namespace FeatherOS::Arch::RealTimeClock
{
namespace
{

// QEMU virt places the PL031 at a fixed address next to the PL011 UART.
constexpr uint64_t Pl031PhysicalAddress = 0x09010000;
constexpr uint64_t Pl031RegisterSize = 0x1000;
constexpr uint32_t RegisterData = 0x000;
constexpr uint32_t RegisterPeripheralId0 = 0xFE0;
constexpr uint32_t RegisterPeripheralId1 = 0xFE4;

volatile uint32_t* Register(uintptr_t base, uint32_t offset)
{
    return reinterpret_cast<volatile uint32_t*>(base + offset);
}

} // namespace

bool ReadUnixSeconds(uint64_t* seconds)
{
    if (seconds == nullptr)
    {
        return false;
    }
    const uintptr_t base = Memory::MapDevice(Pl031PhysicalAddress, Pl031RegisterSize);
    // The PL031 peripheral ID is 0x?41031; check the part number before trusting the counter.
    if (base == 0 || (*Register(base, RegisterPeripheralId0) & 0xFF) != 0x31 ||
        (*Register(base, RegisterPeripheralId1) & 0xFF) != 0x10)
    {
        return false;
    }
    // The data register counts seconds since 1970-01-01 UTC.
    *seconds = *Register(base, RegisterData);
    return true;
}

} // namespace FeatherOS::Arch::RealTimeClock
