#ifndef FEATHEROS_VIRTUAL_MEMORY_H
#define FEATHEROS_VIRTUAL_MEMORY_H

#include <stdint.h>

namespace FeatherOS::Memory
{

enum class PageFlags : uint8_t
{
    Read = 1,
    Write = 2,
    Execute = 4,
    User = 8,
    // Uncached, strongly ordered device registers; kernel mappings only.
    Device = 16,
};

struct AddressSpace
{
    uint64_t root_physical_address;
};

constexpr PageFlags operator|(PageFlags left, PageFlags right)
{
    return static_cast<PageFlags>(static_cast<uint8_t>(left) | static_cast<uint8_t>(right));
}

bool InitializeVirtualMemory(uint64_t hhdm_offset);
bool MapPage(uint64_t virtual_address, uint64_t physical_address, PageFlags flags);
bool UnmapPage(uint64_t virtual_address);
bool CreateAddressSpace(AddressSpace* address_space);
bool DestroyAddressSpace(AddressSpace* address_space);
bool MapPage(AddressSpace& address_space, uint64_t virtual_address, uint64_t physical_address,
             PageFlags flags);
bool ProtectPage(AddressSpace& address_space, uint64_t virtual_address, PageFlags flags);
// Resolves a present, user-accessible 4 KiB page; fails for unmapped and kernel-only addresses.
bool TranslateUserPage(const AddressSpace& address_space, uint64_t virtual_address,
                       uint64_t* physical_address, bool* writable);
bool ActivateAddressSpace(const AddressSpace& address_space);
void ActivateKernelAddressSpace();

} // namespace FeatherOS::Memory

#endif