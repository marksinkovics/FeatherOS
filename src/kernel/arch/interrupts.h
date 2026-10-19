#ifndef FEATHEROS_ARCH_INTERRUPTS_H
#define FEATHEROS_ARCH_INTERRUPTS_H

#include <stdint.h>

namespace FeatherOS::Arch::Interrupts
{

uint64_t SaveAndDisable();
void Restore(uint64_t flags);
// Unmasks interrupts, e.g. while a syscall handler waits for input.
void Enable();

} // namespace FeatherOS::Arch::Interrupts

#endif