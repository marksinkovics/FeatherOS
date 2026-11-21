#ifndef FEATHEROS_AARCH64_INTERRUPTS_H
#define FEATHEROS_AARCH64_INTERRUPTS_H

#include <stdint.h>

namespace FeatherOS::Arch::AArch64
{

using InterruptHandler = void (*)();
using ContextHandler = void (*)(void* context);

bool InitializeInterruptController(const void* device_tree);
void RegisterInterruptHandler(uint32_t interrupt_id, InterruptHandler handler);
void RegisterInterruptHandler(uint32_t interrupt_id, ContextHandler handler, void* context);
bool HasInterruptHandler(uint32_t interrupt_id);
// An SPI configured this way is also routed to the boot CPU; one never configured keeps what the
// firmware and the GIC reset left. Call before registering the handler, which enables it.
void ConfigureEdge(uint32_t interrupt_id);
void ConfigureLevel(uint32_t interrupt_id);
void TriggerSoftwareInterrupt(uint8_t interrupt_id);

} // namespace FeatherOS::Arch::AArch64

#endif