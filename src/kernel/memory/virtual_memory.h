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
};

constexpr PageFlags operator|(PageFlags left, PageFlags right)
{
    return static_cast<PageFlags>(static_cast<uint8_t>(left) | static_cast<uint8_t>(right));
}

bool InitializeVirtualMemory(uint64_t hhdm_offset);
bool MapPage(uint64_t virtual_address, uint64_t physical_address, PageFlags flags);
bool UnmapPage(uint64_t virtual_address);

} // namespace FeatherOS::Memory

#endif