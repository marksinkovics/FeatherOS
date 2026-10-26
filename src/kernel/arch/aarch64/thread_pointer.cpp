#include "kernel/arch/thread_pointer.h"

namespace FeatherOS::Arch::ThreadPointer
{

// EL0 may write TPIDR_EL0 itself, so a switch saves it rather than trusting a kernel copy.
uint64_t Read()
{
    uint64_t value;
    asm volatile("mrs %0, tpidr_el0" : "=r"(value));
    return value;
}

void Write(uint64_t value)
{
    asm volatile("msr tpidr_el0, %0" : : "r"(value));
}

} // namespace FeatherOS::Arch::ThreadPointer
