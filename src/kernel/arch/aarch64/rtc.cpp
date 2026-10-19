#include "kernel/arch/rtc.h"

#include <stddef.h>

namespace FeatherOS::Arch::RealTimeClock
{
namespace
{

// QEMU virt places the PL031 at a fixed address next to the PL011 UART. Like the UART, it is
// reached through Limine's identity map, which every address space shares.
constexpr uint64_t Pl031PhysicalAddress = 0x09010000;
constexpr uint32_t RegisterData = 0x000;
constexpr uint32_t RegisterPeripheralId0 = 0xFE0;
constexpr uint32_t RegisterPeripheralId1 = 0xFE4;

volatile uint32_t* Register(uint32_t offset)
{
    return reinterpret_cast<volatile uint32_t*>(
        static_cast<uintptr_t>(Pl031PhysicalAddress + offset));
}

} // namespace

bool ReadUnixSeconds(uint64_t* seconds)
{
    if (seconds == nullptr)
    {
        return false;
    }
    // The PL031 peripheral ID is 0x?41031; check the part number before trusting the counter.
    if ((*Register(RegisterPeripheralId0) & 0xFF) != 0x31 ||
        (*Register(RegisterPeripheralId1) & 0xFF) != 0x10)
    {
        return false;
    }
    // The data register counts seconds since 1970-01-01 UTC.
    *seconds = *Register(RegisterData);
    return true;
}

} // namespace FeatherOS::Arch::RealTimeClock
