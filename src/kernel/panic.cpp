#include "panic.h"

#include "kernel/arch/serial.h"

#include <new>

namespace FeatherOS
{

__attribute__((noreturn)) void Panic(const char* reason)
{
    // The panicking code may already hold the serial lock.
    Serial::ForceUnlock();
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

// The C++ library's headers call this where they would throw.
void std::__detail::LibraryAbort(const char* message) noexcept
{
    FeatherOS::Panic(message);
}
