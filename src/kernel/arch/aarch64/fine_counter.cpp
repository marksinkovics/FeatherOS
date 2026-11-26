#include "kernel/arch/fine_counter.h"

namespace FeatherOS::Arch::FineCounter
{
namespace
{

uint64_t frequency = 0;

} // namespace

bool Initialize()
{
    asm volatile("mrs %0, CNTFRQ_EL0" : "=r"(frequency));
    return frequency != 0;
}

uint64_t Read()
{
    // Without the barrier the read may happen before earlier instructions finish.
    uint64_t value;
    asm volatile("isb\n\tmrs %0, CNTVCT_EL0" : "=r"(value) : : "memory");
    return value;
}

uint64_t Frequency()
{
    return frequency;
}

} // namespace FeatherOS::Arch::FineCounter
