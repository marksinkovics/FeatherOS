#ifndef FEATHEROS_ARCH_INTERRUPTS_H
#define FEATHEROS_ARCH_INTERRUPTS_H

#include <stdint.h>

namespace FeatherOS::Arch::Interrupts
{

uint64_t SaveAndDisable();
void Restore(uint64_t flags);
// Unmasks interrupts, e.g. while a syscall handler waits for input.
void Enable();
void WaitForInterrupt();

using IpiHandler = void (*)();
void InitializeSecondaryCpu();
void SetIpiHandler(IpiHandler handler);
void SendIpi(uint32_t cpu_index);

} // namespace FeatherOS::Arch::Interrupts

#endif