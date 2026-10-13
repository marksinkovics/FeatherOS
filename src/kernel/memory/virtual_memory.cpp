#include "virtual_memory.h"

#if defined(ARCH_x86_64)
#include "kernel/arch/x86_64/virtual_memory.h"
#elif defined(ARCH_aarch64)
#include "kernel/arch/aarch64/virtual_memory.h"
#endif

namespace FeatherOS::Memory
{

bool InitializeVirtualMemory(uint64_t hhdm_offset)
{
#if defined(ARCH_x86_64)
    return Arch::X86_64::InitializeVirtualMemory(hhdm_offset);
#elif defined(ARCH_aarch64)
    return Arch::AArch64::InitializeVirtualMemory(hhdm_offset);
#else
#error "Unsupported architecture for virtual memory initialization"
#endif
}

bool MapPage(uint64_t virtual_address, uint64_t physical_address, PageFlags flags)
{
    const uint8_t supported_flags = static_cast<uint8_t>(PageFlags::Read) |
                                    static_cast<uint8_t>(PageFlags::Write) |
                                    static_cast<uint8_t>(PageFlags::Execute);
    if ((static_cast<uint8_t>(flags) & static_cast<uint8_t>(PageFlags::Read)) == 0 ||
        (static_cast<uint8_t>(flags) & ~supported_flags) != 0)
    {
        return false;
    }
#if defined(ARCH_x86_64)
    return Arch::X86_64::MapPage(virtual_address, physical_address, flags);
#elif defined(ARCH_aarch64)
    return Arch::AArch64::MapPage(virtual_address, physical_address, flags);
#else
#error "Unsupported architecture for virtual memory mapping"
#endif
}

bool UnmapPage(uint64_t virtual_address)
{
#if defined(ARCH_x86_64)
    return Arch::X86_64::UnmapPage(virtual_address);
#elif defined(ARCH_aarch64)
    return Arch::AArch64::UnmapPage(virtual_address);
#else
#error "Unsupported architecture for virtual memory unmapping"
#endif
}

} // namespace FeatherOS::Memory