#include "cpu.h"

#include "kernel/panic.h"

#include <stddef.h>

namespace FeatherOS::Cpu
{
namespace
{

struct LocalState
{
    uint32_t index;
};

LocalState local_states[MaximumCpus];

#if defined(ARCH_x86_64)
constexpr uint32_t GsBaseMsr = 0xC0000101;
constexpr uint32_t KernelGsBaseMsr = 0xC0000102;

void WriteMsr(uint32_t msr, uint64_t value)
{
    asm volatile("wrmsr"
                 :
                 : "c"(msr), "a"(static_cast<uint32_t>(value)),
                   "d"(static_cast<uint32_t>(value >> 32))
                 : "memory");
}
#endif

} // namespace

void InstallLocalState(uint32_t cpu_index)
{
    if (cpu_index >= MaximumCpus)
    {
        Panic("CPU index exceeds the supported CPU count");
    }
    LocalState* state = &local_states[cpu_index];
    state->index = cpu_index;
#if defined(ARCH_x86_64)
    // The kernel's GS base points at this CPU's state. SWAPGS exchanges it with KERNEL_GS_BASE,
    // which holds the user GS base (always 0 here), on every ring 3 <-> ring 0 transition.
    WriteMsr(GsBaseMsr, reinterpret_cast<uint64_t>(state));
    WriteMsr(KernelGsBaseMsr, 0);
#elif defined(ARCH_aarch64)
    // TPIDR_EL1 is not accessible from EL0, so no switching is needed on user entry.
    asm volatile("msr TPIDR_EL1, %0" : : "r"(state) : "memory");
#else
#error "Unsupported architecture for CPU-local state"
#endif
}

uint32_t CurrentIndex()
{
#if defined(ARCH_x86_64)
    uint32_t index;
    asm volatile("movl %%gs:%c1, %0" : "=r"(index) : "i"(offsetof(LocalState, index)));
    return index;
#elif defined(ARCH_aarch64)
    const LocalState* state;
    asm volatile("mrs %0, TPIDR_EL1" : "=r"(state));
    return state->index;
#endif
}

} // namespace FeatherOS::Cpu
