#ifndef FEATHEROS_AARCH64_VIRTUAL_MEMORY_H
#define FEATHEROS_AARCH64_VIRTUAL_MEMORY_H

#include "kernel/memory/virtual_memory.h"

namespace FeatherOS::Memory::Arch::AArch64
{

bool InitializeVirtualMemory(uint64_t hhdm_offset);
bool MapPage(uint64_t virtual_address, uint64_t physical_address, PageFlags flags);
bool UnmapPage(uint64_t virtual_address);

} // namespace FeatherOS::Memory::Arch::AArch64

#endif