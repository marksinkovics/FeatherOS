#include "kernel_test.h"

#include "kernel/arch/serial.h"
#include "kernel/panic.h"

#if defined(ARCH_x86_64) && defined(FEATHEROS_INTERRUPT_TEST)
#include "kernel/arch/x86_64/interrupts.h"
#elif defined(ARCH_aarch64) && defined(FEATHEROS_INTERRUPT_TEST)
#include "kernel/arch/aarch64/interrupts.h"
#include <limine.h>
#endif

#include <stdint.h>

#if defined(ARCH_aarch64) && defined(FEATHEROS_INTERRUPT_TEST)
namespace
{

__attribute__((used, section(".limine_requests"))) volatile limine_dtb_request dtb_request = {
    .id = LIMINE_DTB_REQUEST, .revision = 0, .response = nullptr};

volatile uint64_t gic_interrupt_count = 0;

void OnGicInterrupt()
{
    ++gic_interrupt_count;
}

} // namespace
#elif defined(ARCH_x86_64) && defined(FEATHEROS_INTERRUPT_TEST)
namespace
{

volatile uint64_t pit_interrupt_count = 0;

void OnPitInterrupt()
{
    ++pit_interrupt_count;
}

} // namespace
#endif

namespace FeatherOS::KernelTest
{

void Run()
{
#if defined(FEATHEROS_PANIC_TEST)
    Panic("Intentional panic test");
#endif

#if defined(FEATHEROS_EXCEPTION_TEST) && defined(ARCH_x86_64)
    asm volatile("xor %%edx, %%edx\n\t"
                 "mov $1, %%eax\n\t"
                 "xor %%ecx, %%ecx\n\t"
                 "divl %%ecx"
                 :
                 :
                 : "rax", "rcx", "rdx", "cc");
#elif defined(FEATHEROS_EXCEPTION_TEST) && defined(ARCH_aarch64)
    asm volatile(".inst 0x00000000");
#endif

#if defined(FEATHEROS_INTERRUPT_TEST) && defined(ARCH_x86_64)
    Arch::X86_64::RegisterInterruptHandler(0, OnPitInterrupt);
    Arch::X86_64::InitializeInterruptController();
    Arch::X86_64::StartPit(100);
    asm volatile("sti" ::: "memory");
    while (pit_interrupt_count == 0)
    {
        asm volatile("hlt" ::: "memory");
    }
    asm volatile("cli" ::: "memory");
    Serial::Write("KERN/IRQ: PIT interrupt acknowledged\n");
#elif defined(FEATHEROS_INTERRUPT_TEST) && defined(ARCH_aarch64)
    if (dtb_request.response == nullptr || dtb_request.response->dtb_ptr == nullptr)
    {
        Panic("AArch64 IRQ test requires a device tree");
    }
    const auto* dtb = static_cast<const uint8_t*>(dtb_request.response->dtb_ptr);
    const uint32_t dtb_magic = (static_cast<uint32_t>(dtb[0]) << 24) |
                               (static_cast<uint32_t>(dtb[1]) << 16) |
                               (static_cast<uint32_t>(dtb[2]) << 8) | dtb[3];
    if (dtb_magic != 0xD00DFEED)
    {
        Panic("Limine provided an invalid device tree");
    }
    Arch::AArch64::RegisterInterruptHandler(0, OnGicInterrupt);
    if (!Arch::AArch64::InitializeInterruptController(dtb))
    {
        Panic("Unsupported AArch64 interrupt controller");
    }
    Arch::AArch64::TriggerSoftwareInterrupt(0);
    asm volatile("msr daifclr, #2" ::: "memory");
    while (gic_interrupt_count == 0)
    {
        asm volatile("wfi" ::: "memory");
    }
    asm volatile("msr daifset, #2" ::: "memory");
    Serial::Write("KERN/IRQ: GIC software interrupt acknowledged\n");
#endif
}

} // namespace FeatherOS::KernelTest