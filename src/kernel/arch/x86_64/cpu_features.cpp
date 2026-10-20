#include "kernel/arch/cpu_features.h"

#include "kernel/cpu.h"

namespace FeatherOS::Arch::CpuFeatures
{
namespace
{

constexpr uint32_t Cr4Smep = 1U << 20;
constexpr uint32_t Cr4Smap = 1U << 21;

Protections protections = {false, false, false};

void Cpuid(uint32_t leaf, uint32_t subleaf, uint32_t* eax, uint32_t* ebx, uint32_t* ecx)
{
    uint32_t edx;
    asm volatile("cpuid" : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(edx) : "a"(leaf), "c"(subleaf));
}

} // namespace

} // namespace FeatherOS::Arch::CpuFeatures

// The interrupt entry stub reads this to decide whether CLAC is a valid instruction.
extern "C"
{
    uint8_t FeatherOS_x86_64_SmapEnabled = 0;
}

namespace FeatherOS::Arch::CpuFeatures
{

void InitializeCurrentCpu()
{
    // Every CPU is the same model, so the boot CPU's CPUID decides for all of them.
    if (Cpu::CurrentIndex() == 0)
    {
        uint32_t max_leaf;
        uint32_t ebx;
        uint32_t ecx;
        Cpuid(0, 0, &max_leaf, &ebx, &ecx);
        uint32_t eax;
        Cpuid(1, 0, &eax, &ebx, &ecx);
        protections.hardware_random = (ecx & (1U << 30)) != 0;
        if (max_leaf >= 7)
        {
            Cpuid(7, 0, &eax, &ebx, &ecx);
            protections.execute_protection = (ebx & (1U << 7)) != 0;
            protections.access_protection = (ebx & (1U << 20)) != 0;
        }
        FeatherOS_x86_64_SmapEnabled = protections.access_protection ? 1 : 0;
    }

    uint64_t cr4;
    asm volatile("mov %%cr4, %0" : "=r"(cr4));
    if (protections.execute_protection)
    {
        cr4 |= Cr4Smep;
    }
    if (protections.access_protection)
    {
        asm volatile("clac" ::: "memory");
        cr4 |= Cr4Smap;
    }
    asm volatile("mov %0, %%cr4" : : "r"(cr4) : "memory");
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
    // RDRAND may briefly run dry; Intel recommends a handful of retries.
    for (uint32_t attempt = 0; attempt < 10; ++attempt)
    {
        uint8_t ok;
        asm volatile("rdrand %0\n\tsetc %1" : "=r"(*value), "=qm"(ok) : : "cc");
        if (ok != 0)
        {
            return true;
        }
    }
    return false;
}

void AllowUserAccess()
{
    if (protections.access_protection)
    {
        asm volatile("stac" ::: "memory");
    }
}

void ForbidUserAccess()
{
    if (protections.access_protection)
    {
        asm volatile("clac" ::: "memory");
    }
}

} // namespace FeatherOS::Arch::CpuFeatures
