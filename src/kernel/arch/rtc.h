#ifndef FEATHEROS_ARCH_RTC_H
#define FEATHEROS_ARCH_RTC_H

#include <stdint.h>

// The platform real-time clock: the CMOS RTC on x86-64 and the PL031 on AArch64 QEMU virt.
namespace FeatherOS::Arch::RealTimeClock
{

// Reads the current UTC time as seconds since 1970-01-01, with one-second resolution.
bool ReadUnixSeconds(uint64_t* seconds);

} // namespace FeatherOS::Arch::RealTimeClock

#endif
