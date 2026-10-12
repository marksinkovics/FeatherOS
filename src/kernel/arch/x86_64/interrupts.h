#ifndef FEATHEROS_X86_64_INTERRUPTS_H
#define FEATHEROS_X86_64_INTERRUPTS_H

#include <stdint.h>

namespace FeatherOS::Arch::X86_64
{

using InterruptHandler = void (*)();

void InitializeInterruptController();
void RegisterInterruptHandler(uint8_t irq, InterruptHandler handler);
void StartPit(uint32_t frequency_hz);

} // namespace FeatherOS::Arch::X86_64

#endif