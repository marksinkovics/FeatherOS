#ifndef FEATHEROS_MEMORY_KERNEL_STACK_H
#define FEATHEROS_MEMORY_KERNEL_STACK_H

#include <stdint.h>

namespace FeatherOS::Memory
{

constexpr uint64_t KernelStackSize = 16384;

uint8_t* AllocateKernelStack();
void FreeKernelStack(uint8_t* stack);

} // namespace FeatherOS::Memory

#endif
