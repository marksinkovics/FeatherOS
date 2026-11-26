#ifndef FEATHEROS_ARCH_FINE_COUNTER_H
#define FEATHEROS_ARCH_FINE_COUNTER_H

#include <stdint.h>

// A free-running counter finer than a timer tick: the TSC on x86-64, the virtual counter on
// AArch64.
namespace FeatherOS::Arch::FineCounter
{

// True when the counter is usable. x86-64 trusts the TSC only with an invariant TSC or under a
// hypervisor, and then measures its frequency against 10 ticks of the running timer, so it must
// run after the timer started. The measurement is made once and kept.
bool Initialize();
uint64_t Read();
uint64_t Frequency();

} // namespace FeatherOS::Arch::FineCounter

#endif
