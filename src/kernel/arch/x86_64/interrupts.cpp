#include "interrupts.h"

#include "kernel/panic.h"

namespace FeatherOS::Arch::X86_64
{
namespace
{

constexpr uint16_t PicMasterCommand = 0x20;
constexpr uint16_t PicMasterData = 0x21;
constexpr uint16_t PicSlaveCommand = 0xA0;
constexpr uint16_t PicSlaveData = 0xA1;
constexpr uint16_t PitChannel0 = 0x40;
constexpr uint16_t PitCommand = 0x43;
constexpr uint32_t PitInputFrequency = 1193182;

InterruptHandler handlers[16] = {};

void WritePort(uint16_t port, uint8_t value)
{
    asm volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

uint8_t ReadPort(uint16_t port)
{
    uint8_t value;
    asm volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

void WaitForIo()
{
    WritePort(0x80, 0);
}

void SendEndOfInterrupt(uint8_t irq)
{
    if (irq >= 8)
    {
        WritePort(PicSlaveCommand, 0x20);
    }
    WritePort(PicMasterCommand, 0x20);
}

} // namespace

void InitializeInterruptController()
{
    WritePort(PicMasterCommand, 0x11);
    WaitForIo();
    WritePort(PicSlaveCommand, 0x11);
    WaitForIo();
    WritePort(PicMasterData, 0x20);
    WaitForIo();
    WritePort(PicSlaveData, 0x28);
    WaitForIo();
    WritePort(PicMasterData, 0x04);
    WaitForIo();
    WritePort(PicSlaveData, 0x02);
    WaitForIo();
    WritePort(PicMasterData, 0x01);
    WaitForIo();
    WritePort(PicSlaveData, 0x01);
    WaitForIo();

    WritePort(PicMasterData, 0xFE);
    WritePort(PicSlaveData, 0xFF);
}

void RegisterInterruptHandler(uint8_t irq, InterruptHandler handler)
{
    if (irq >= 16 || handler == nullptr)
    {
        Panic("Invalid x86 interrupt handler registration");
    }
    handlers[irq] = handler;
}

void UnmaskIrq(uint8_t irq)
{
    if (irq >= 16)
    {
        Panic("Invalid x86 IRQ unmask request");
    }
    if (irq >= 8)
    {
        WritePort(PicSlaveData, ReadPort(PicSlaveData) & ~(1U << (irq - 8)));
        WritePort(PicMasterData, ReadPort(PicMasterData) & ~(1U << 2));
    }
    else
    {
        WritePort(PicMasterData, ReadPort(PicMasterData) & ~(1U << irq));
    }
}

void StartPit(uint32_t frequency_hz)
{
    if (frequency_hz == 0 || frequency_hz > PitInputFrequency)
    {
        Panic("Invalid PIT frequency");
    }

    const uint16_t divisor = static_cast<uint16_t>(PitInputFrequency / frequency_hz);
    WritePort(PitCommand, 0x36);
    WritePort(PitChannel0, divisor & 0xFF);
    WritePort(PitChannel0, divisor >> 8);
}

void StopPit()
{
    asm volatile("cli" ::: "memory");
    WritePort(PicMasterData, ReadPort(PicMasterData) | 1);
}

extern "C" void FeatherOS_x86_64_HandleHardwareInterrupt(uint64_t vector)
{
    const uint8_t irq = static_cast<uint8_t>(vector - 32);
    if (irq >= 16)
    {
        Panic("Invalid x86 hardware interrupt vector");
    }
    SendEndOfInterrupt(irq);
    if (handlers[irq] != nullptr)
    {
        handlers[irq]();
    }
}

} // namespace FeatherOS::Arch::X86_64

namespace FeatherOS::Arch::Interrupts
{

uint64_t SaveAndDisable()
{
    uint64_t flags;
    asm volatile("pushfq; popq %0; cli" : "=r"(flags) : : "memory");
    return flags;
}

void Restore(uint64_t flags)
{
    if ((flags & (1U << 9)) != 0)
    {
        asm volatile("sti" : : : "memory");
    }
}

void Enable()
{
    asm volatile("sti" : : : "memory");
}

void WaitForInterrupt()
{
    // STI takes effect after the next instruction, so no interrupt can slip in before HLT.
    asm volatile("sti; hlt" : : : "memory");
}

} // namespace FeatherOS::Arch::Interrupts