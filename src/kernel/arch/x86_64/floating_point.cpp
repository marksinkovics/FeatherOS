#include "kernel/arch/floating_point.h"

namespace FeatherOS::Arch::FloatingPoint
{
namespace
{

constexpr uint64_t Cr0MonitorCoprocessor = 1U << 1;
constexpr uint64_t Cr0Emulation = 1U << 2;
constexpr uint64_t Cr0TaskSwitched = 1U << 3;
// FXSAVE/FXRSTOR and SSE instructions, and SIMD exceptions reported as #XM.
constexpr uint64_t Cr4FxsaveSupport = 1U << 9;
constexpr uint64_t Cr4SimdExceptions = 1U << 10;
// The values FNINIT and a reset leave: every exception masked, round to nearest, and 64-bit
// x87 precision.
constexpr uint16_t DefaultControlWord = 0x037F;
constexpr uint32_t DefaultMxcsr = 0x1F80;
constexpr uint32_t ControlWordOffset = 0;
constexpr uint32_t MxcsrOffset = 24;

} // namespace

void InitializeCurrentCpu()
{
    uint64_t cr0;
    asm volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 = (cr0 | Cr0MonitorCoprocessor) & ~(Cr0Emulation | Cr0TaskSwitched);
    asm volatile("mov %0, %%cr0" : : "r"(cr0) : "memory");
    uint64_t cr4;
    asm volatile("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= Cr4FxsaveSupport | Cr4SimdExceptions;
    asm volatile("mov %0, %%cr4" : : "r"(cr4) : "memory");
    asm volatile("fninit" ::: "memory");
}

void InitializeState(State* state)
{
    for (uint8_t& byte : state->bytes)
    {
        byte = 0;
    }
    state->bytes[ControlWordOffset] = static_cast<uint8_t>(DefaultControlWord);
    state->bytes[ControlWordOffset + 1] = static_cast<uint8_t>(DefaultControlWord >> 8);
    for (uint32_t index = 0; index < 4; ++index)
    {
        state->bytes[MxcsrOffset + index] = static_cast<uint8_t>(DefaultMxcsr >> (8 * index));
    }
}

void Save(State* state)
{
    asm volatile("fxsave64 %0" : "=m"(*state) : : "memory");
}

void Restore(const State* state)
{
    asm volatile("fxrstor64 %0" : : "m"(*state) : "memory");
}

} // namespace FeatherOS::Arch::FloatingPoint
