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
    const uint8_t supported_flags =
        static_cast<uint8_t>(PageFlags::Read) | static_cast<uint8_t>(PageFlags::Write) |
        static_cast<uint8_t>(PageFlags::Execute) | static_cast<uint8_t>(PageFlags::Device);
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

bool CreateAddressSpace(AddressSpace* address_space)
{
    if (address_space == nullptr)
    {
        return false;
    }
#if defined(ARCH_x86_64)
    return Arch::X86_64::CreateAddressSpace(address_space);
#elif defined(ARCH_aarch64)
    return Arch::AArch64::CreateAddressSpace(address_space);
#else
#error "Unsupported architecture for address-space creation"
#endif
}

bool DestroyAddressSpace(AddressSpace* address_space)
{
    if (address_space == nullptr || address_space->root_physical_address == 0)
    {
        return false;
    }
#if defined(ARCH_x86_64)
    return Arch::X86_64::DestroyAddressSpace(address_space);
#elif defined(ARCH_aarch64)
    return Arch::AArch64::DestroyAddressSpace(address_space);
#else
#error "Unsupported architecture for address-space destruction"
#endif
}

bool MapPage(AddressSpace& address_space, uint64_t virtual_address, uint64_t physical_address,
             PageFlags flags)
{
    constexpr uint8_t supported_flags =
        static_cast<uint8_t>(PageFlags::Read) | static_cast<uint8_t>(PageFlags::Write) |
        static_cast<uint8_t>(PageFlags::Execute) | static_cast<uint8_t>(PageFlags::User);
    if ((static_cast<uint8_t>(flags) & static_cast<uint8_t>(PageFlags::Read)) == 0 ||
        (static_cast<uint8_t>(flags) & static_cast<uint8_t>(PageFlags::User)) == 0 ||
        (static_cast<uint8_t>(flags) & ~supported_flags) != 0)
    {
        return false;
    }
#if defined(ARCH_x86_64)
    return Arch::X86_64::MapPage(address_space, virtual_address, physical_address, flags);
#elif defined(ARCH_aarch64)
    return Arch::AArch64::MapPage(address_space, virtual_address, physical_address, flags);
#else
#error "Unsupported architecture for address-space mapping"
#endif
}

bool UnmapUserPage(AddressSpace& address_space, uint64_t virtual_address,
                   uint64_t* physical_address)
{
    if (physical_address == nullptr)
    {
        return false;
    }
#if defined(ARCH_x86_64)
    return Arch::X86_64::UnmapUserPage(address_space, virtual_address, physical_address);
#elif defined(ARCH_aarch64)
    return Arch::AArch64::UnmapUserPage(address_space, virtual_address, physical_address);
#else
#error "Unsupported architecture for user page unmapping"
#endif
}

bool ActivateAddressSpace(const AddressSpace& address_space)
{
#if defined(ARCH_x86_64)
    return Arch::X86_64::ActivateAddressSpace(address_space);
#elif defined(ARCH_aarch64)
    return Arch::AArch64::ActivateAddressSpace(address_space);
#else
#error "Unsupported architecture for address-space activation"
#endif
}

bool ProtectPage(AddressSpace& address_space, uint64_t virtual_address, PageFlags flags)
{
    constexpr uint8_t supported_flags =
        static_cast<uint8_t>(PageFlags::Read) | static_cast<uint8_t>(PageFlags::Write) |
        static_cast<uint8_t>(PageFlags::Execute) | static_cast<uint8_t>(PageFlags::User);
    if ((static_cast<uint8_t>(flags) & static_cast<uint8_t>(PageFlags::Read)) == 0 ||
        (static_cast<uint8_t>(flags) & static_cast<uint8_t>(PageFlags::User)) == 0 ||
        (static_cast<uint8_t>(flags) & ~supported_flags) != 0)
    {
        return false;
    }
#if defined(ARCH_x86_64)
    return Arch::X86_64::ProtectPage(address_space, virtual_address, flags);
#elif defined(ARCH_aarch64)
    return Arch::AArch64::ProtectPage(address_space, virtual_address, flags);
#else
#error "Unsupported architecture for user page protection"
#endif
}

bool TranslateUserPage(const AddressSpace& address_space, uint64_t virtual_address,
                       uint64_t* physical_address, bool* writable)
{
    if (physical_address == nullptr || writable == nullptr)
    {
        return false;
    }
#if defined(ARCH_x86_64)
    return Arch::X86_64::TranslateUserPage(address_space, virtual_address, physical_address,
                                           writable);
#elif defined(ARCH_aarch64)
    return Arch::AArch64::TranslateUserPage(address_space, virtual_address, physical_address,
                                            writable);
#else
#error "Unsupported architecture for user page translation"
#endif
}

void ActivateKernelAddressSpace()
{
#if defined(ARCH_x86_64)
    Arch::X86_64::ActivateKernelAddressSpace();
#elif defined(ARCH_aarch64)
    Arch::AArch64::ActivateKernelAddressSpace();
#else
#error "Unsupported architecture for kernel address-space activation"
#endif
}

} // namespace FeatherOS::Memory