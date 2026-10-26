#include "local_apic.h"
#include "virtual_memory.h"

#include "kernel/arch/interrupts.h"
#include "kernel/cpu.h"
#include "kernel/memory/device_memory.h"
#include "kernel/panic.h"
#include "kernel/time/timer.h"

namespace FeatherOS::Arch::X86_64::LocalApic
{
namespace
{

constexpr uint32_t ApicBaseMsr = 0x1B;
constexpr uint64_t ApicBaseAddressMask = 0x000FFFFFFFFFF000;
constexpr uint64_t ApicRegisterSize = 0x1000;

constexpr uint32_t IdRegister = 0x020;
constexpr uint32_t EndOfInterruptRegister = 0x0B0;
constexpr uint32_t SpuriousVectorRegister = 0x0F0;
constexpr uint32_t InterruptCommandLow = 0x300;
constexpr uint32_t InterruptCommandHigh = 0x310;
constexpr uint32_t TimerVectorRegister = 0x320;
constexpr uint32_t Lint0Register = 0x350;
constexpr uint32_t Lint1Register = 0x360;
constexpr uint32_t TimerInitialCount = 0x380;
constexpr uint32_t TimerCurrentCount = 0x390;
constexpr uint32_t TimerDivideConfiguration = 0x3E0;

constexpr uint32_t ApicSoftwareEnable = 1U << 8;
constexpr uint32_t DeliveryPending = 1U << 12;
constexpr uint32_t LevelAssert = 1U << 14;
constexpr uint32_t LvtMasked = 1U << 16;
constexpr uint32_t TimerPeriodic = 1U << 17;
constexpr uint32_t DeliveryExtInt = 0x700;
constexpr uint32_t DeliveryNmi = 0x400;
constexpr uint32_t TimerDivideBy16 = 0x3;

uintptr_t register_base = 0;
uint32_t timer_initial_count = 0;
uint32_t apic_ids[Cpu::MaximumCpus] = {};
Handler ipi_handler = nullptr;
Handler timer_handler = nullptr;

volatile uint32_t& Register(uint32_t offset)
{
    return *reinterpret_cast<volatile uint32_t*>(register_base + offset);
}

uint64_t ReadMsr(uint32_t msr)
{
    uint32_t low;
    uint32_t high;
    asm volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(msr));
    return (static_cast<uint64_t>(high) << 32) | low;
}

void EnableCurrentCpu()
{
    Register(SpuriousVectorRegister) = ApicSoftwareEnable | SpuriousVector;
    apic_ids[Cpu::CurrentIndex()] = Register(IdRegister) >> 24;
}

} // namespace

bool InitializeBootCpu()
{
    if (register_base != 0)
    {
        return true;
    }
    register_base = Memory::MapDevice(ReadMsr(ApicBaseMsr) & ApicBaseAddressMask, ApicRegisterSize);
    if (register_base == 0)
    {
        return false;
    }
    EnableCurrentCpu();
    // Virtual-wire mode: the legacy PIC still reaches the boot CPU through LINT0.
    Register(Lint0Register) = DeliveryExtInt;
    Register(Lint1Register) = DeliveryNmi;
    return true;
}

void InitializeSecondaryCpu()
{
    if (register_base == 0)
    {
        Panic("Local APIC is not mapped before secondary CPU startup");
    }
    EnableCurrentCpu();
    Register(Lint0Register) = LvtMasked;
    Register(Lint1Register) = DeliveryNmi;
}

void SetIpiHandler(Handler handler)
{
    ipi_handler = handler;
}

void SendIpi(uint32_t cpu_index)
{
    if (cpu_index >= Cpu::MaximumCpus)
    {
        Panic("IPI target CPU index is out of range");
    }
    // The high and low writes form one command; an interrupt between them could send its own.
    const uint64_t flags = Interrupts::SaveAndDisable();
    while ((Register(InterruptCommandLow) & DeliveryPending) != 0)
    {
    }
    Register(InterruptCommandHigh) = apic_ids[cpu_index] << 24;
    Register(InterruptCommandLow) = LevelAssert | IpiVector;
    Interrupts::Restore(flags);
}

bool CalibrateTimer(uint32_t calibration_ticks)
{
    if (register_base == 0 || calibration_ticks == 0)
    {
        return false;
    }
    Register(TimerDivideConfiguration) = TimerDivideBy16;
    Register(TimerVectorRegister) = LvtMasked | TimerVector;
    Time::WaitForNextTick();
    Register(TimerInitialCount) = UINT32_MAX;
    for (uint32_t tick = 0; tick < calibration_ticks; ++tick)
    {
        Time::WaitForNextTick();
    }
    const uint32_t elapsed = UINT32_MAX - Register(TimerCurrentCount);
    Register(TimerInitialCount) = 0;
    timer_initial_count = elapsed / calibration_ticks;
    return timer_initial_count != 0;
}

bool StartPeriodicTimer(Handler handler)
{
    if (timer_initial_count == 0 || handler == nullptr)
    {
        return false;
    }
    timer_handler = handler;
    Register(TimerDivideConfiguration) = TimerDivideBy16;
    Register(TimerVectorRegister) = TimerPeriodic | TimerVector;
    Register(TimerInitialCount) = timer_initial_count;
    return true;
}

void HandleInterrupt(uint64_t vector)
{
    if (vector == SpuriousVector)
    {
        return;
    }
    // Acknowledge first: the handler may switch to another task before returning.
    Register(EndOfInterruptRegister) = 0;
    // A TLB shootdown also arrives as an IPI; it is served before the handler may switch tasks.
    if (vector == IpiVector)
    {
        FeatherOS::Memory::Arch::X86_64::ServiceShootDown();
    }
    const Handler handler = vector == IpiVector ? ipi_handler : timer_handler;
    if (handler != nullptr)
    {
        handler();
    }
}

} // namespace FeatherOS::Arch::X86_64::LocalApic
