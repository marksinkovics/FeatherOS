#include "kernel/arch/cpu_features.h"

#include "kernel/cpu.h"

namespace FeatherOS::Arch::CpuFeatures
{
namespace
{

constexpr uint64_t SctlrSetPrivilegedAccessNever = 1ULL << 23;

Protections protections = {false, false, false};

// Encoded directly: the kernel targets ARMv8.0, whose assembler does not know these ARMv8.1+
// instructions.
void SetPrivilegedAccessNever()
{
    asm volatile(".inst 0xd500419f" ::: "memory"); // msr PAN, #1
}

void ClearPrivilegedAccessNever()
{
    asm volatile(".inst 0xd500409f" ::: "memory"); // msr PAN, #0
}

} // namespace

void InitializeCurrentCpu()
{
    // Every CPU is the same model, so the boot CPU's ID registers decide for all of them.
    if (Cpu::CurrentIndex() == 0)
    {
        uint64_t memory_features;
        uint64_t instruction_features;
        asm volatile("mrs %0, ID_AA64MMFR1_EL1" : "=r"(memory_features));
        asm volatile("mrs %0, ID_AA64ISAR0_EL1" : "=r"(instruction_features));
        protections.access_protection = ((memory_features >> 20) & 0xF) != 0;
        protections.hardware_random = ((instruction_features >> 60) & 0xF) != 0;
        protections.execute_protection = false;
    }
    if (protections.access_protection)
    {
        // With SPAN clear, taking an exception sets PAN, so handlers start without user access
        // even if they interrupted an access window.
        uint64_t control;
        asm volatile("mrs %0, SCTLR_EL1" : "=r"(control));
        control &= ~SctlrSetPrivilegedAccessNever;
        asm volatile("msr SCTLR_EL1, %0\n\tisb" : : "r"(control) : "memory");
        SetPrivilegedAccessNever();
    }
}

Protections Enabled()
{
    return protections;
}

bool ReadHardwareRandom(uint64_t* value)
{
    if (!protections.hardware_random)
    {
        return false;
    }
    uint64_t failed;
    // RNDR sets the Z flag when it could not produce a value.
    asm volatile("mrs %0, s3_3_c2_c4_0\n\tcset %1, eq" : "=r"(*value), "=r"(failed) : : "cc");
    return failed == 0;
}

void AllowUserAccess()
{
    if (protections.access_protection)
    {
        ClearPrivilegedAccessNever();
    }
}

void ForbidUserAccess()
{
    if (protections.access_protection)
    {
        SetPrivilegedAccessNever();
    }
}

} // namespace FeatherOS::Arch::CpuFeatures
