#include "halt.h"

namespace FeatherOS
{

[[noreturn]] void HaltForever()
{
    for (;;)
    {
#if defined(ARCH_x86_64)
        asm volatile("hlt");
#elif defined(ARCH_aarch64)
        asm volatile("wfi");
#else
#error "Unsupported architecture halt instruction"
#endif
    }
}

} // namespace FeatherOS