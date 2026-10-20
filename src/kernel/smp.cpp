#include "smp.h"

#include "kernel/arch/cpu_features.h"
#include "kernel/arch/interrupts.h"
#include "kernel/arch/serial.h"
#include "kernel/cpu.h"
#include "kernel/exception.h"
#include "kernel/panic.h"
#include "kernel/scheduler.h"
#include "kernel/string/format.h"
#include "kernel/time/timer.h"
#include "kernel/sync/spinlock.h"

#include <limine.h>

namespace FeatherOS::Smp
{
namespace
{

// Limine has already started every CPU, so each one only has to notice its goto_address.
constexpr uint64_t StartupSpinLimit = 100000000;

constexpr uint64_t IpiTimeoutTicks = 10;
constexpr uint64_t CpuTimerTicks = 3;
constexpr uint64_t CpuTimerTimeoutTicks = 50;

uint32_t online_cpu_count = 1;
uint32_t ipi_counts[Cpu::MaximumCpus] = {};

void OnIpi()
{
    __atomic_fetch_add(&ipi_counts[Cpu::CurrentIndex()], 1U, __ATOMIC_RELAXED);
}

[[noreturn]] void SecondaryCpuEntry(limine_smp_info* info)
{
    const auto cpu_index = static_cast<uint32_t>(info->extra_argument);
    Cpu::InstallLocalState(cpu_index);
    InitializeSecondaryCpuExceptions(cpu_index);
    Arch::CpuFeatures::InitializeCurrentCpu();
    if (Cpu::CurrentIndex() != cpu_index)
    {
        Panic("CPU-local state does not match the started CPU");
    }
    char message[32];
    if (!String::FormatTo(message, sizeof(message), "KERN/I: CPU {} online\n", cpu_index))
    {
        Panic("CPU online message formatting failed");
    }
    Arch::Interrupts::InitializeSecondaryCpu();
    if (!Time::StartSecondaryCpuTimer())
    {
        Panic("Secondary CPU timer did not start");
    }
    Serial::Write(message);
    __atomic_fetch_add(&online_cpu_count, 1U, __ATOMIC_RELEASE);
    Scheduler::RunSecondaryCpu();
}

void VerifyIpisAndTimers(uint32_t cpu_count)
{
    for (uint32_t cpu_index = 1; cpu_index < cpu_count; ++cpu_index)
    {
        Arch::Interrupts::SendIpi(cpu_index);
        const uint64_t deadline = Time::TickCount() + IpiTimeoutTicks;
        while (__atomic_load_n(&ipi_counts[cpu_index], __ATOMIC_RELAXED) == 0 &&
               Time::TickCount() < deadline)
        {
            Sync::CpuRelax();
        }
        if (__atomic_load_n(&ipi_counts[cpu_index], __ATOMIC_RELAXED) == 0)
        {
            Panic("A secondary CPU did not receive its IPI");
        }
    }

    const uint64_t deadline = Time::TickCount() + CpuTimerTimeoutTicks;
    for (uint32_t cpu_index = 1; cpu_index < cpu_count; ++cpu_index)
    {
        while (Time::CpuTickCount(cpu_index) < CpuTimerTicks && Time::TickCount() < deadline)
        {
            Time::WaitForNextTick();
        }
        if (Time::CpuTickCount(cpu_index) < CpuTimerTicks)
        {
            Panic("A secondary CPU timer is not ticking");
        }
    }
}

bool IsBootCpu(const limine_smp_response& response, const limine_smp_info& cpu)
{
#if defined(ARCH_x86_64)
    return cpu.lapic_id == response.bsp_lapic_id;
#elif defined(ARCH_aarch64)
    return cpu.mpidr == response.bsp_mpidr;
#else
#error "Unsupported architecture for SMP startup"
#endif
}

} // namespace

uint32_t StartSecondaryCpus(limine_smp_response* response)
{
    if (response == nullptr)
    {
        return OnlineCpuCount();
    }
    // Registered first so each CPU enables the IPI while it initializes.
    Arch::Interrupts::SetIpiHandler(OnIpi);
    if (!Time::PrepareSecondaryCpuTimers())
    {
        Panic("Could not prepare secondary CPU timers");
    }

    uint32_t cpu_count = 1;
    for (uint64_t index = 0; index < response->cpu_count && cpu_count < Cpu::MaximumCpus; ++index)
    {
        limine_smp_info* cpu = response->cpus[index];
        if (IsBootCpu(*response, *cpu))
        {
            continue;
        }
        cpu->extra_argument = cpu_count++;
        // Release ordering publishes extra_argument before the CPU sees its entry point.
        __atomic_store_n(&cpu->goto_address, &SecondaryCpuEntry, __ATOMIC_RELEASE);
    }

    for (uint64_t spin = 0; spin < StartupSpinLimit && OnlineCpuCount() < cpu_count; ++spin)
    {
        Sync::CpuRelax();
    }
    if (OnlineCpuCount() != cpu_count)
    {
        Panic("A secondary CPU did not start");
    }
    VerifyIpisAndTimers(cpu_count);
    return cpu_count;
}

uint32_t OnlineCpuCount()
{
    return __atomic_load_n(&online_cpu_count, __ATOMIC_ACQUIRE);
}

} // namespace FeatherOS::Smp
