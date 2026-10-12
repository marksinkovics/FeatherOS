#include "panic.h"

#include "kernel/arch/serial.h"

namespace FeatherOS
{

__attribute__((noreturn)) void Panic(const char* reason)
{
    Serial::Write("KERN/PANIC: ");
    Serial::Write(reason == nullptr ? "Unknown reason" : reason);
    Serial::Write("\n");

    for (;;)
    {
#if defined(ARCH_x86_64)
        asm volatile("cli; hlt" ::: "memory");
#elif defined(ARCH_aarch64)
        asm volatile("msr daifset, #0xf; wfi" ::: "memory");
#else
#error "Unsupported architecture panic halt instruction"
#endif
    }
}

} // namespace FeatherOS