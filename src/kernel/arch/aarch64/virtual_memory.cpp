#include "virtual_memory.h"

#include "kernel/cpu.h"
#include "kernel/memory/page_allocator.h"
#include "kernel/sync/spinlock.h"

#include <stddef.h>

namespace FeatherOS::Memory::Arch::AArch64
{
namespace
{

constexpr uint64_t PageAddressMask = 0x0000FFFFFFFFF000;
constexpr uint64_t MairNormalWriteBack = 0xFF;
constexpr uint64_t MairDeviceNgnrne = 0x00;
constexpr uint64_t DescriptorValid = 1;
constexpr uint64_t DescriptorTableOrPage = 1U << 1;
constexpr uint64_t AccessFlag = 1U << 10;
constexpr uint64_t InnerShareable = 3U << 8;
constexpr uint64_t AccessPermissionReadOnly = 1U << 7;
constexpr uint64_t AccessPermissionUser = 1U << 6;
constexpr uint64_t PrivilegedExecuteNever = 1ULL << 53;
constexpr uint64_t UnprivilegedExecuteNever = 1ULL << 54;

uint64_t hhdm_offset = 0;
volatile uint64_t* root_table = nullptr;
uint64_t kernel_ttbr0 = 0;
// Kernel tables are shared by every address space and CPU; held while any table is edited.
constinit FeatherOS::Sync::SpinLock page_table_lock;
uint64_t active_root_physical_addresses[FeatherOS::Cpu::MaximumCpus] = {};

bool IsActiveOnAnyCpu(uint64_t root_physical_address)
{
    for (const uint64_t active_root : active_root_physical_addresses)
    {
        if (active_root == root_physical_address)
        {
            return true;
        }
    }
    return false;
}
uint8_t normal_memory_attribute_index = 0;
uint8_t device_memory_attribute_index = 0;

bool IsCanonical(uint64_t address)
{
    const uint64_t upper = address >> 48;
    return ((address >> 47) & 1) == 0 ? upper == 0 : upper == 0xFFFF;
}

volatile uint64_t* PhysicalTable(uint64_t physical_address)
{
    if (physical_address > UINT64_MAX - hhdm_offset)
    {
        return nullptr;
    }
    const uint64_t virtual_address = hhdm_offset + physical_address;
    if (virtual_address > UINTPTR_MAX)
    {
        return nullptr;
    }
    return reinterpret_cast<volatile uint64_t*>(static_cast<uintptr_t>(virtual_address));
}

void InvalidatePage(uint64_t virtual_address);

bool GetOrCreateTable(volatile uint64_t* parent, size_t index, volatile uint64_t** child)
{
    const uint64_t entry = parent[index];
    if ((entry & DescriptorValid) != 0)
    {
        if ((entry & DescriptorTableOrPage) == 0)
        {
            return false;
        }
        *child = PhysicalTable(entry & PageAddressMask);
        return *child != nullptr;
    }

    uint64_t physical_address;
    if (!AllocatePageFrame(&physical_address))
    {
        return false;
    }
    volatile uint64_t* table = PhysicalTable(physical_address);
    if (table == nullptr)
    {
        FreePageFrame(physical_address);
        return false;
    }
    for (size_t entry_index = 0; entry_index < 512; ++entry_index)
    {
        table[entry_index] = 0;
    }
    parent[index] = (physical_address & PageAddressMask) | DescriptorValid | DescriptorTableOrPage;
    *child = table;
    return true;
}

bool MapPageInRoot(volatile uint64_t* selected_root, uint64_t virtual_address,
                   uint64_t physical_address, PageFlags flags, bool user_accessible)
{
    if (selected_root == nullptr || !IsCanonical(virtual_address) ||
        (user_accessible && virtual_address >= (1ULL << 48)) || virtual_address % PageSize != 0 ||
        physical_address % PageSize != 0 || (physical_address & ~PageAddressMask) != 0)
    {
        return false;
    }

    const size_t level0_index = (virtual_address >> 39) & 0x1FF;
    const size_t level1_index = (virtual_address >> 30) & 0x1FF;
    const size_t level2_index = (virtual_address >> 21) & 0x1FF;
    const size_t level3_index = (virtual_address >> 12) & 0x1FF;

    volatile uint64_t* level1;
    volatile uint64_t* level2;
    volatile uint64_t* level3;
    if (!GetOrCreateTable(selected_root, level0_index, &level1) ||
        !GetOrCreateTable(level1, level1_index, &level2) ||
        !GetOrCreateTable(level2, level2_index, &level3))
    {
        return false;
    }
    if ((level3[level3_index] & DescriptorValid) != 0)
    {
        return false;
    }

    const bool device =
        (static_cast<uint8_t>(flags) & static_cast<uint8_t>(PageFlags::Device)) != 0;
    const uint64_t attribute_index =
        device ? device_memory_attribute_index : normal_memory_attribute_index;
    uint64_t descriptor = (physical_address & PageAddressMask) | DescriptorValid |
                          DescriptorTableOrPage | AccessFlag | InnerShareable |
                          (attribute_index << 2);
    if ((static_cast<uint8_t>(flags) & static_cast<uint8_t>(PageFlags::Write)) == 0)
    {
        descriptor |= AccessPermissionReadOnly;
    }
    if (user_accessible)
    {
        // The kernel never runs user code, so EL1 may not execute any user page.
        descriptor |= AccessPermissionUser | PrivilegedExecuteNever;
    }
    if ((static_cast<uint8_t>(flags) & static_cast<uint8_t>(PageFlags::Execute)) == 0)
    {
        descriptor |= PrivilegedExecuteNever | UnprivilegedExecuteNever;
    }
    level3[level3_index] = descriptor;
    InvalidatePage(virtual_address);
    return true;
}

void InvalidatePage(uint64_t virtual_address)
{
    const uint64_t operand = virtual_address >> 12;
    asm volatile("dsb ishst\n\ttlbi vaae1is, %0\n\tdsb ish\n\tisb" : : "r"(operand) : "memory");
}

bool DestroyUserPageTable(uint64_t physical_address, size_t level)
{
    volatile uint64_t* table = PhysicalTable(physical_address);
    if (table == nullptr || level == 0)
    {
        return false;
    }
    for (size_t index = 0; index < 512; ++index)
    {
        const uint64_t descriptor = table[index];
        if ((descriptor & DescriptorValid) == 0)
        {
            continue;
        }
        const uint64_t child_physical_address = descriptor & PageAddressMask;
        table[index] = 0;
        if (level == 1)
        {
            if (!FreePageFrame(child_physical_address))
            {
                return false;
            }
        }
        else if ((descriptor & DescriptorTableOrPage) == 0 ||
                 !DestroyUserPageTable(child_physical_address, level - 1) ||
                 !FreePageFrame(child_physical_address))
        {
            return false;
        }
    }
    return true;
}

} // namespace

bool InitializeVirtualMemory(uint64_t offset)
{
    hhdm_offset = offset;
    uint64_t mair;
    asm volatile("mrs %0, MAIR_EL1" : "=r"(mair));
    bool found_normal_memory_attribute = false;
    bool found_device_memory_attribute = false;
    for (uint8_t index = 0; index < 8; ++index)
    {
        const uint64_t attribute = (mair >> (index * 8)) & 0xFF;
        if (!found_normal_memory_attribute && attribute == MairNormalWriteBack)
        {
            normal_memory_attribute_index = index;
            found_normal_memory_attribute = true;
        }
        else if (!found_device_memory_attribute && attribute == MairDeviceNgnrne)
        {
            device_memory_attribute_index = index;
            found_device_memory_attribute = true;
        }
    }
    if (!found_normal_memory_attribute || !found_device_memory_attribute)
    {
        return false;
    }

    uint64_t ttbr1;
    asm volatile("mrs %0, TTBR1_EL1" : "=r"(ttbr1));
    root_table = PhysicalTable(ttbr1 & PageAddressMask);
    asm volatile("mrs %0, TTBR0_EL1" : "=r"(kernel_ttbr0));
    active_root_physical_addresses[FeatherOS::Cpu::CurrentIndex()] = kernel_ttbr0 & PageAddressMask;
    return root_table != nullptr;
}

bool MapPage(uint64_t virtual_address, uint64_t physical_address, PageFlags flags)
{
    FeatherOS::Sync::SpinLockGuard guard(page_table_lock);
    return MapPageInRoot(root_table, virtual_address, physical_address, flags, false);
}

bool CreateAddressSpace(AddressSpace* address_space)
{
    FeatherOS::Sync::SpinLockGuard guard(page_table_lock);
    if (address_space == nullptr || root_table == nullptr)
    {
        return false;
    }
    uint64_t root_physical_address;
    if (!AllocatePageFrame(&root_physical_address) ||
        (root_physical_address & ~PageAddressMask) != 0)
    {
        return false;
    }
    volatile uint64_t* new_root = PhysicalTable(root_physical_address);
    if (new_root == nullptr)
    {
        FreePageFrame(root_physical_address);
        return false;
    }
    // The kernel lives in TTBR1 and maps devices there, so user roots hold only user pages.
    for (size_t index = 0; index < 512; ++index)
    {
        new_root[index] = 0;
    }
    address_space->root_physical_address = root_physical_address;
    return true;
}

bool DestroyAddressSpace(AddressSpace* address_space)
{
    FeatherOS::Sync::SpinLockGuard guard(page_table_lock);
    if (address_space == nullptr || address_space->root_physical_address == 0 ||
        IsActiveOnAnyCpu(address_space->root_physical_address) ||
        address_space->root_physical_address == (kernel_ttbr0 & PageAddressMask) ||
        (address_space->root_physical_address & ~PageAddressMask) != 0)
    {
        return false;
    }
    volatile uint64_t* user_root = PhysicalTable(address_space->root_physical_address);
    if (user_root == nullptr)
    {
        return false;
    }
    for (size_t index = 0; index < 512; ++index)
    {
        const uint64_t descriptor = user_root[index];
        if ((descriptor & DescriptorValid) == 0)
        {
            continue;
        }
        if ((descriptor & DescriptorTableOrPage) == 0)
        {
            return false;
        }
        const uint64_t table_physical_address = descriptor & PageAddressMask;
        user_root[index] = 0;
        if (!DestroyUserPageTable(table_physical_address, 3) ||
            !FreePageFrame(table_physical_address))
        {
            return false;
        }
    }
    if (!FreePageFrame(address_space->root_physical_address))
    {
        return false;
    }
    address_space->root_physical_address = 0;
    return true;
}

bool MapPage(AddressSpace& address_space, uint64_t virtual_address, uint64_t physical_address,
             PageFlags flags)
{
    FeatherOS::Sync::SpinLockGuard guard(page_table_lock);
    if (address_space.root_physical_address == 0 ||
        (address_space.root_physical_address & ~PageAddressMask) != 0)
    {
        return false;
    }
    volatile uint64_t* user_root = PhysicalTable(address_space.root_physical_address);
    if (user_root == nullptr)
    {
        return false;
    }
    return MapPageInRoot(user_root, virtual_address, physical_address, flags, true);
}

bool ProtectPage(AddressSpace& address_space, uint64_t virtual_address, PageFlags flags)
{
    FeatherOS::Sync::SpinLockGuard guard(page_table_lock);
    if (address_space.root_physical_address == 0 ||
        (address_space.root_physical_address & ~PageAddressMask) != 0 ||
        !IsCanonical(virtual_address) || virtual_address >= (1ULL << 48) ||
        virtual_address % PageSize != 0)
    {
        return false;
    }

    const size_t level0_index = (virtual_address >> 39) & 0x1FF;
    const size_t level1_index = (virtual_address >> 30) & 0x1FF;
    const size_t level2_index = (virtual_address >> 21) & 0x1FF;
    const size_t level3_index = (virtual_address >> 12) & 0x1FF;
    volatile uint64_t* level0 = PhysicalTable(address_space.root_physical_address);
    if (level0 == nullptr)
    {
        return false;
    }

    volatile uint64_t* level1;
    volatile uint64_t* level2;
    volatile uint64_t* level3;
    const uint64_t entry0 = level0[level0_index];
    if ((entry0 & (DescriptorValid | DescriptorTableOrPage)) !=
        (DescriptorValid | DescriptorTableOrPage))
    {
        return false;
    }
    level1 = PhysicalTable(entry0 & PageAddressMask);
    if (level1 == nullptr)
    {
        return false;
    }
    const uint64_t entry1 = level1[level1_index];
    if ((entry1 & (DescriptorValid | DescriptorTableOrPage)) !=
        (DescriptorValid | DescriptorTableOrPage))
    {
        return false;
    }
    level2 = PhysicalTable(entry1 & PageAddressMask);
    if (level2 == nullptr)
    {
        return false;
    }
    const uint64_t entry2 = level2[level2_index];
    if ((entry2 & (DescriptorValid | DescriptorTableOrPage)) !=
        (DescriptorValid | DescriptorTableOrPage))
    {
        return false;
    }
    level3 = PhysicalTable(entry2 & PageAddressMask);
    if (level3 == nullptr)
    {
        return false;
    }

    uint64_t descriptor = level3[level3_index];
    if ((descriptor & DescriptorValid) == 0 || (descriptor & AccessPermissionUser) == 0)
    {
        return false;
    }
    descriptor &= ~(AccessPermissionReadOnly | UnprivilegedExecuteNever);
    descriptor |= PrivilegedExecuteNever;
    if ((static_cast<uint8_t>(flags) & static_cast<uint8_t>(PageFlags::Write)) == 0)
    {
        descriptor |= AccessPermissionReadOnly;
    }
    if ((static_cast<uint8_t>(flags) & static_cast<uint8_t>(PageFlags::Execute)) == 0)
    {
        descriptor |= PrivilegedExecuteNever | UnprivilegedExecuteNever;
    }
    level3[level3_index] = descriptor;
    InvalidatePage(virtual_address);
    return true;
}

bool TranslateUserPage(const AddressSpace& address_space, uint64_t virtual_address,
                       uint64_t* physical_address, bool* writable)
{
    if (virtual_address >= (1ULL << 48) || address_space.root_physical_address == 0 ||
        (address_space.root_physical_address & ~PageAddressMask) != 0)
    {
        return false;
    }

    const size_t table_indices[] = {
        (virtual_address >> 39) & 0x1FF,
        (virtual_address >> 30) & 0x1FF,
        (virtual_address >> 21) & 0x1FF,
    };
    volatile uint64_t* level = PhysicalTable(address_space.root_physical_address);
    for (size_t table_index = 0; level != nullptr && table_index < 3; ++table_index)
    {
        const uint64_t entry = level[table_indices[table_index]];
        if ((entry & (DescriptorValid | DescriptorTableOrPage)) !=
            (DescriptorValid | DescriptorTableOrPage))
        {
            return false;
        }
        level = PhysicalTable(entry & PageAddressMask);
    }
    if (level == nullptr)
    {
        return false;
    }

    const uint64_t descriptor = level[(virtual_address >> 12) & 0x1FF];
    if ((descriptor & DescriptorValid) == 0 || (descriptor & AccessPermissionUser) == 0)
    {
        return false;
    }
    *physical_address = (descriptor & PageAddressMask) | (virtual_address & (PageSize - 1));
    *writable = (descriptor & AccessPermissionReadOnly) == 0;
    return true;
}

bool UnmapUserPage(AddressSpace& address_space, uint64_t virtual_address,
                   uint64_t* physical_address)
{
    FeatherOS::Sync::SpinLockGuard guard(page_table_lock);
    if (virtual_address >= (1ULL << 48) || virtual_address % PageSize != 0 ||
        address_space.root_physical_address == 0 ||
        (address_space.root_physical_address & ~PageAddressMask) != 0)
    {
        return false;
    }

    const size_t table_indices[] = {
        (virtual_address >> 39) & 0x1FF,
        (virtual_address >> 30) & 0x1FF,
        (virtual_address >> 21) & 0x1FF,
    };
    volatile uint64_t* level = PhysicalTable(address_space.root_physical_address);
    for (size_t table_index = 0; level != nullptr && table_index < 3; ++table_index)
    {
        const uint64_t entry = level[table_indices[table_index]];
        if ((entry & (DescriptorValid | DescriptorTableOrPage)) !=
            (DescriptorValid | DescriptorTableOrPage))
        {
            return false;
        }
        level = PhysicalTable(entry & PageAddressMask);
    }
    if (level == nullptr)
    {
        return false;
    }

    const size_t level3_index = (virtual_address >> 12) & 0x1FF;
    const uint64_t descriptor = level[level3_index];
    if ((descriptor & DescriptorValid) == 0 || (descriptor & AccessPermissionUser) == 0)
    {
        return false;
    }
    level[level3_index] = 0;
    InvalidatePage(virtual_address);
    *physical_address = descriptor & PageAddressMask;
    return true;
}

// Nothing to send: UnmapUserPage's tlbi vaae1is is broadcast to every CPU in the inner-shareable
// domain, and its dsb ish waits until they have all dropped the translation.
void ShootDownUserMappings(const AddressSpace&) {}

bool ActivateAddressSpace(const AddressSpace& address_space)
{
    if (address_space.root_physical_address == 0 ||
        (address_space.root_physical_address & ~PageAddressMask) != 0)
    {
        return false;
    }
    const uint64_t root = address_space.root_physical_address;
    asm volatile("dsb ishst\n\tmsr TTBR0_EL1, %0\n\ttlbi vmalle1\n\t"
                 "dsb ish\n\tisb"
                 :
                 : "r"(root)
                 : "memory");
    active_root_physical_addresses[FeatherOS::Cpu::CurrentIndex()] = root;
    return true;
}

void ActivateKernelAddressSpace()
{
    const uint64_t root = kernel_ttbr0;
    asm volatile("dsb ishst\n\tmsr TTBR0_EL1, %0\n\ttlbi vmalle1\n\t"
                 "dsb ish\n\tisb"
                 :
                 : "r"(root)
                 : "memory");
    active_root_physical_addresses[FeatherOS::Cpu::CurrentIndex()] = root & PageAddressMask;
}

bool UnmapPage(uint64_t virtual_address)
{
    FeatherOS::Sync::SpinLockGuard guard(page_table_lock);
    if (root_table == nullptr || !IsCanonical(virtual_address) || virtual_address % PageSize != 0)
    {
        return false;
    }

    const size_t level0_index = (virtual_address >> 39) & 0x1FF;
    const size_t level1_index = (virtual_address >> 30) & 0x1FF;
    const size_t level2_index = (virtual_address >> 21) & 0x1FF;
    const size_t level3_index = (virtual_address >> 12) & 0x1FF;
    uint64_t entry = root_table[level0_index];
    if ((entry & (DescriptorValid | DescriptorTableOrPage)) !=
        (DescriptorValid | DescriptorTableOrPage))
    {
        return false;
    }
    volatile uint64_t* level1 = PhysicalTable(entry & PageAddressMask);
    if (level1 == nullptr)
    {
        return false;
    }
    entry = level1[level1_index];
    if ((entry & (DescriptorValid | DescriptorTableOrPage)) !=
        (DescriptorValid | DescriptorTableOrPage))
    {
        return false;
    }
    volatile uint64_t* level2 = PhysicalTable(entry & PageAddressMask);
    if (level2 == nullptr)
    {
        return false;
    }
    entry = level2[level2_index];
    if ((entry & (DescriptorValid | DescriptorTableOrPage)) !=
        (DescriptorValid | DescriptorTableOrPage))
    {
        return false;
    }
    volatile uint64_t* level3 = PhysicalTable(entry & PageAddressMask);
    if (level3 == nullptr || (level3[level3_index] & DescriptorValid) == 0)
    {
        return false;
    }

    level3[level3_index] = 0;
    InvalidatePage(virtual_address);
    return true;
}

} // namespace FeatherOS::Memory::Arch::AArch64
