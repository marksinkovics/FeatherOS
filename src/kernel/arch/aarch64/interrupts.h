#ifndef FEATHEROS_AARCH64_INTERRUPTS_H
#define FEATHEROS_AARCH64_INTERRUPTS_H

#include <stdint.h>

namespace FeatherOS::Arch::AArch64
{

using InterruptHandler = void (*)();

bool InitializeInterruptController(const void* device_tree);
void RegisterInterruptHandler(uint32_t interrupt_id, InterruptHandler handler);
void TriggerSoftwareInterrupt(uint8_t interrupt_id);

} // namespace FeatherOS::Arch::AArch64

#endif