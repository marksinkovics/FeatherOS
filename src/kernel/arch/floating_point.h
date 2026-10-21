#ifndef FEATHEROS_ARCH_FLOATING_POINT_H
#define FEATHEROS_ARCH_FLOATING_POINT_H

#include <stdint.h>

// User programs' floating-point and SIMD registers. The kernel is built without them
// (-mgeneral-regs-only on x86-64, +nofp+nosimd on AArch64), so they keep the running user task's
// values through interrupts and syscalls, and only a context switch has to save and load them.
namespace FeatherOS::Arch::FloatingPoint
{

#if defined(ARCH_x86_64)
// The FXSAVE image: x87 and MXCSR state, then xmm0-xmm15.
constexpr uint32_t StateSize = 512;
#elif defined(ARCH_aarch64)
// q0-q31, then FPCR and FPSR.
constexpr uint32_t StateSize = 528;
#endif

struct alignas(16) State
{
    uint8_t bytes[StateSize];
};

// Lets the current CPU run floating-point and SIMD instructions in user mode.
void InitializeCurrentCpu();
// The state a new task starts with: round to nearest, every exception masked.
void InitializeState(State* state);
void Save(State* state);
void Restore(const State* state);

} // namespace FeatherOS::Arch::FloatingPoint

#endif
