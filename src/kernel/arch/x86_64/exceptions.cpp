#include "exceptions.h"

#include "kernel/exception.h"

#include <stddef.h>
#include <stdint.h>

namespace FeatherOS::Arch::X86_64
{
namespace
{

struct IdtEntry
{
    uint16_t offset_low;
    uint16_t selector;
    uint8_t ist;
    uint8_t attributes;
    uint16_t offset_middle;
    uint32_t offset_high;
    uint32_t reserved;
} __attribute__((packed));

struct IdtRegister
{
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

struct TaskStateSegment
{
    uint32_t reserved0;
    uint64_t rsp0;
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t reserved1;
    uint64_t ist[7];
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;
} __attribute__((packed));

struct ExceptionContext
{
    uint64_t r15;
    uint64_t r14;
    uint64_t r13;
    uint64_t r12;
    uint64_t r11;
    uint64_t r10;
    uint64_t r9;
    uint64_t r8;
    uint64_t rbp;
    uint64_t rdi;
    uint64_t rsi;
    uint64_t rdx;
    uint64_t rcx;
    uint64_t rbx;
    uint64_t rax;
    uint64_t vector;
    uint64_t error_code;
    uint64_t instruction_pointer;
    uint64_t code_segment;
    uint64_t flags;
};

IdtEntry idt[256];
alignas(16) uint64_t gdt[5];
TaskStateSegment tss;
alignas(16) uint8_t kernel_stack[16384];
alignas(16) uint8_t double_fault_stack[16384];
alignas(16) uint8_t nmi_stack[16384];
alignas(16) uint8_t machine_check_stack[16384];
alignas(16) uint8_t test_exception_stack[16384];

extern "C" void* x86_64_exception_stub_table[];
extern "C" void* x86_64_irq_stub_table[];
extern "C" void x86_64_load_gdt(const IdtRegister* descriptor);
extern "C" void FeatherOS_x86_64_HandleHardwareInterrupt(uint64_t vector);

const char* ExceptionName(uint64_t vector)
{
    static constexpr const char* names[] = {
        "Divide error",
        "Debug",
        "Non-maskable interrupt",
        "Breakpoint",
        "Overflow",
        "Bound range exceeded",
        "Invalid opcode",
        "Device not available",
        "Double fault",
        "Coprocessor segment overrun",
        "Invalid TSS",
        "Segment not present",
        "Stack-segment fault",
        "General protection fault",
        "Page fault",
        "Reserved",
        "x87 floating-point exception",
        "Alignment check",
        "Machine check",
        "SIMD floating-point exception",
        "Virtualization exception",
        "Control protection exception",
        "Reserved",
        "Reserved",
        "Reserved",
        "Reserved",
        "Reserved",
        "Reserved",
        "Hypervisor injection exception",
        "VMM communication exception",
        "Security exception",
        "Reserved",
    };
    return vector < sizeof(names) / sizeof(names[0]) ? names[vector] : "Unknown exception";
}

void SetEntry(size_t vector, uintptr_t handler, uint16_t selector, uint8_t ist)
{
    idt[vector] = {
        .offset_low = static_cast<uint16_t>(handler & 0xFFFF),
        .selector = selector,
        .ist = ist,
        .attributes = 0x8E,
        .offset_middle = static_cast<uint16_t>((handler >> 16) & 0xFFFF),
        .offset_high = static_cast<uint32_t>(handler >> 32),
        .reserved = 0,
    };
}

} // namespace

void InitializeExceptions()
{
    asm volatile("cli" ::: "memory");

    tss.rsp0 = reinterpret_cast<uint64_t>(kernel_stack + sizeof(kernel_stack));
    tss.ist[0] = reinterpret_cast<uint64_t>(double_fault_stack + sizeof(double_fault_stack));
    tss.ist[1] = reinterpret_cast<uint64_t>(nmi_stack + sizeof(nmi_stack));
    tss.ist[2] = reinterpret_cast<uint64_t>(machine_check_stack + sizeof(machine_check_stack));
    tss.ist[3] = reinterpret_cast<uint64_t>(test_exception_stack + sizeof(test_exception_stack));
    tss.iomap_base = sizeof(tss);

    gdt[1] = 0x00AF9A000000FFFF;
    gdt[2] = 0x00CF92000000FFFF;
    const uint64_t tss_base = reinterpret_cast<uint64_t>(&tss);
    const uint64_t tss_limit = sizeof(tss) - 1;
    gdt[3] = (tss_limit & 0xFFFF) | ((tss_base & 0xFFFFFF) << 16) | (0x89ULL << 40) |
             (((tss_limit >> 16) & 0xF) << 48) | (((tss_base >> 24) & 0xFF) << 56);
    gdt[4] = tss_base >> 32;
    const IdtRegister gdt_register = {
        .limit = sizeof(gdt) - 1,
        .base = reinterpret_cast<uint64_t>(gdt),
    };
    x86_64_load_gdt(&gdt_register);

    for (size_t vector = 0; vector < 32; ++vector)
    {
        uint8_t ist = 0;
        if (vector == 8)
        {
            ist = 1;
        }
        else if (vector == 2)
        {
            ist = 2;
        }
        else if (vector == 18)
        {
            ist = 3;
        }
#if defined(FEATHEROS_EXCEPTION_TEST)
        else if (vector == 0)
        {
            ist = 4;
        }
#endif
        SetEntry(vector, reinterpret_cast<uintptr_t>(x86_64_exception_stub_table[vector]), 0x08,
                 ist);
    }
    for (size_t irq = 0; irq < 16; ++irq)
    {
        SetEntry(32 + irq, reinterpret_cast<uintptr_t>(x86_64_irq_stub_table[irq]), 0x08, 0);
    }

    const IdtRegister idt_register = {
        .limit = sizeof(idt) - 1,
        .base = reinterpret_cast<uint64_t>(idt),
    };
    asm volatile("lidt %0" : : "m"(idt_register));
}

extern "C" void FeatherOS_x86_64_ExceptionDispatch(const ExceptionContext* context)
{
    if (context->vector >= 32 && context->vector < 48)
    {
        FeatherOS_x86_64_HandleHardwareInterrupt(context->vector);
        return;
    }

    FeatherOS::ExceptionReport report;
    report.architecture = FeatherOS::ExceptionArchitecture::X86_64;
    report.name = ExceptionName(context->vector);
    report.vector = context->vector;
    report.instruction_pointer = context->instruction_pointer;
    report.error_code = context->error_code;
    report.syndrome = 0;
    report.fault_address = 0;
    FeatherOS::ReportFatalException(report);
}

} // namespace FeatherOS::Arch::X86_64