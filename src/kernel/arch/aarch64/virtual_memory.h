#ifndef FEATHEROS_AARCH64_VIRTUAL_MEMORY_H
#define FEATHEROS_AARCH64_VIRTUAL_MEMORY_H

#include "kernel/memory/virtual_memory.h"

namespace FeatherOS::Memory::Arch::AArch64
{

bool InitializeVirtualMemory(uint64_t hhdm_offset);
bool MapPage(uint64_t virtual_address, uint64_t physical_address, PageFlags flags);
bool UnmapPage(uint64_t virtual_address);
bool CreateAddressSpace(AddressSpace* address_space);
bool DestroyAddressSpace(AddressSpace* address_space);
bool MapPage(AddressSpace& address_space, uint64_t virtual_address, uint64_t physical_address,
             PageFlags flags);
bool ProtectPage(AddressSpace& address_space, uint64_t virtual_address, PageFlags flags);
bool TranslateUserPage(const AddressSpace& address_space, uint64_t virtual_address,
                       uint64_t* physical_address, bool* writable);
bool UnmapUserPage(AddressSpace& address_space, uint64_t virtual_address,
                   uint64_t* physical_address);
void ShootDownUserMappings(const AddressSpace& address_space);
bool ActivateAddressSpace(const AddressSpace& address_space);
void ActivateKernelAddressSpace();

} // namespace FeatherOS::Memory::Arch::AArch64

#endif