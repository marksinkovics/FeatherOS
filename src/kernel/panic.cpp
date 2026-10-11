#include "panic.h"

#include "kernel/arch/x86_64/serial.h"

namespace FeatherOS
{

__attribute__((noreturn)) void Panic(const char* reason)
{
    Serial::Init();
    Serial::Write("KERN/PANIC: ");
    Serial::Write(reason == nullptr ? "Unknown reason" : reason);
    Serial::Write("\n");

    for (;;)
    {
        asm volatile("cli; hlt" ::: "memory");
    }
}

} // namespace FeatherOS