#include "virtual_memory.h"

#include "kernel/cpu.h"
#include "kernel/memory/page_allocator.h"
#include "kernel/sync/spinlock.h"

#include <stddef.h>

namespace FeatherOS::Memory::Arch::X86_64
{
namespace
{

constexpr uint64_t PageAddressMask = 0x000FFFFFFFFFF000;
constexpr uint64_t Present = 1;
constexpr uint64_t Writable = 1U << 1;
constexpr uint64_t UserAccessible = 1U << 2;
constexpr uint64_t WriteThrough = 1U << 3;
constexpr uint64_t CacheDisable = 1U << 4;
constexpr uint64_t LargePage = 1U << 7;
constexpr uint64_t NoExecute = 1ULL << 63;
constexpr uint64_t EferMsr = 0xC0000080;
constexpr uint64_t EferNoExecuteEnable = 1U << 11;

uint64_t hhdm_offset = 0;
volatile uint64_t* root_table = nullptr;
uint64_t kernel_root_physical_address = 0;
// Kernel tables are shared by every address space and CPU; held while any table is edited.
FeatherOS::Sync::SpinLock page_table_lock;
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

bool GetOrCreateTable(volatile uint64_t* parent, size_t index, volatile uint64_t** child,
                      bool user_accessible)
{
    uint64_t entry = parent[index];
    if ((entry & Present) != 0)
    {
        if ((entry & LargePage) != 0)
        {
            return false;
        }
        parent[index] = entry | Present | Writable | (user_accessible ? UserAccessible : 0);
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
    parent[index] = (physical_address & PageAddressMask) | Present | Writable |
                    (user_accessible ? UserAccessible : 0);
    *child = table;
    return true;
}

bool MapPageInRoot(volatile uint64_t* selected_root, uint64_t virtual_address,
                   uint64_t physical_address, PageFlags flags, bool user_accessible)
{
    if (selected_root == nullptr || !IsCanonical(virtual_address) ||
        virtual_address % PageSize != 0 || physical_address % PageSize != 0 ||
        (physical_address & ~PageAddressMask) != 0)
    {
        return false;
    }

    const size_t pml4_index = (virtual_address >> 39) & 0x1FF;
    const size_t pdpt_index = (virtual_address >> 30) & 0x1FF;
    const size_t pd_index = (virtual_address >> 21) & 0x1FF;
    const size_t pt_index = (virtual_address >> 12) & 0x1FF;

    volatile uint64_t* pdpt;
    volatile uint64_t* page_directory;
    volatile uint64_t* page_table;
    if (!GetOrCreateTable(selected_root, pml4_index, &pdpt, user_accessible) ||
        !GetOrCreateTable(pdpt, pdpt_index, &page_directory, user_accessible) ||
        !GetOrCreateTable(page_directory, pd_index, &page_table, user_accessible))
    {
        return false;
    }
    if ((page_table[pt_index] & Present) != 0)
    {
        return false;
    }

    uint64_t entry = (physical_address & PageAddressMask) | Present;
    if ((static_cast<uint8_t>(flags) & static_cast<uint8_t>(PageFlags::Write)) != 0)
    {
        entry |= Writable;
    }
    if (user_accessible)
    {
        entry |= UserAccessible;
    }
    if ((static_cast<uint8_t>(flags) & static_cast<uint8_t>(PageFlags::Execute)) == 0)
    {
        entry |= NoExecute;
    }
    if ((static_cast<uint8_t>(flags) & static_cast<uint8_t>(PageFlags::Device)) != 0)
    {
        entry |= CacheDisable | WriteThrough;
    }
    page_table[pt_index] = entry;
    InvalidatePage(virtual_address);
    return true;
}

void InvalidatePage(uint64_t virtual_address)
{
    asm volatile("invlpg (%0)" : : "r"(virtual_address) : "memory");
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
        const uint64_t entry = table[index];
        if ((entry & Present) == 0)
        {
            continue;
        }
        const uint64_t child_physical_address = entry & PageAddressMask;
        table[index] = 0;
        if (level == 1)
        {
            if (!FreePageFrame(child_physical_address))
            {
                return false;
            }
        }
        else
        {
            if ((entry & LargePage) != 0 ||
                !DestroyUserPageTable(child_physical_address, level - 1) ||
                !FreePageFrame(child_physical_address))
            {
                return false;
            }
        }
    }
    return true;
}

} // namespace

bool InitializeVirtualMemory(uint64_t offset)
{
    hhdm_offset = offset;
    uint64_t cr3;
    asm volatile("mov %%cr3, %0" : "=r"(cr3));
    kernel_root_physical_address = cr3 & PageAddressMask;
    active_root_physical_addresses[FeatherOS::Cpu::CurrentIndex()] = kernel_root_physical_address;
    root_table = PhysicalTable(kernel_root_physical_address);
    if (root_table == nullptr)
    {
        return false;
    }
    // Address spaces copy the kernel's top-level entries when created, so every kernel-half
    // entry must exist now; later kernel mappings then appear in every address space.
    for (size_t index = 256; index < 512; ++index)
    {
        volatile uint64_t* table;
        if (!GetOrCreateTable(root_table, index, &table, false))
        {
            return false;
        }
    }

    uint32_t msr = EferMsr;
    uint32_t low;
    uint32_t high;
    asm volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(msr));
    uint64_t efer = (static_cast<uint64_t>(high) << 32) | low;
    efer |= EferNoExecuteEnable;
    low = static_cast<uint32_t>(efer);
    high = static_cast<uint32_t>(efer >> 32);
    asm volatile("wrmsr" : : "a"(low), "d"(high), "c"(msr) : "memory");
    return true;
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
    for (size_t index = 0; index < 512; ++index)
    {
        new_root[index] = index >= 256 ? root_table[index] : 0;
    }
    address_space->root_physical_address = root_physical_address;
    return true;
}

bool DestroyAddressSpace(AddressSpace* address_space)
{
    FeatherOS::Sync::SpinLockGuard guard(page_table_lock);
    if (address_space == nullptr || address_space->root_physical_address == 0 ||
        address_space->root_physical_address == kernel_root_physical_address ||
        IsActiveOnAnyCpu(address_space->root_physical_address) ||
        (address_space->root_physical_address & ~PageAddressMask) != 0)
    {
        return false;
    }
    volatile uint64_t* user_root = PhysicalTable(address_space->root_physical_address);
    if (user_root == nullptr)
    {
        return false;
    }
    for (size_t index = 0; index < 256; ++index)
    {
        const uint64_t entry = user_root[index];
        if ((entry & Present) == 0)
        {
            continue;
        }
        if ((entry & LargePage) != 0)
        {
            return false;
        }
        const uint64_t table_physical_address = entry & PageAddressMask;
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
    if (virtual_address >= 0x0000800000000000 || address_space.root_physical_address == 0 ||
        (address_space.root_physical_address & ~PageAddressMask) != 0)
    {
        return false;
    }
    volatile uint64_t* user_root = PhysicalTable(address_space.root_physical_address);
    return MapPageInRoot(user_root, virtual_address, physical_address, flags, true);
}

bool ProtectPage(AddressSpace& address_space, uint64_t virtual_address, PageFlags flags)
{
    FeatherOS::Sync::SpinLockGuard guard(page_table_lock);
    if (virtual_address >= 0x0000800000000000 || !IsCanonical(virtual_address) ||
        virtual_address % PageSize != 0 || address_space.root_physical_address == 0 ||
        (address_space.root_physical_address & ~PageAddressMask) != 0)
    {
        return false;
    }

    const size_t pml4_index = (virtual_address >> 39) & 0x1FF;
    const size_t pdpt_index = (virtual_address >> 30) & 0x1FF;
    const size_t pd_index = (virtual_address >> 21) & 0x1FF;
    const size_t pt_index = (virtual_address >> 12) & 0x1FF;
    volatile uint64_t* level = PhysicalTable(address_space.root_physical_address);
    if (level == nullptr)
    {
        return false;
    }

    const size_t table_indices[] = {pml4_index, pdpt_index, pd_index};
    for (size_t table_index = 0; table_index < 3; ++table_index)
    {
        const uint64_t entry = level[table_indices[table_index]];
        if ((entry & Present) == 0 || (entry & LargePage) != 0 || (entry & UserAccessible) == 0)
        {
            return false;
        }
        level = PhysicalTable(entry & PageAddressMask);
        if (level == nullptr)
        {
            return false;
        }
    }

    uint64_t entry = level[pt_index];
    if ((entry & Present) == 0 || (entry & UserAccessible) == 0)
    {
        return false;
    }
    entry &= ~(Writable | NoExecute);
    if ((static_cast<uint8_t>(flags) & static_cast<uint8_t>(PageFlags::Write)) != 0)
    {
        entry |= Writable;
    }
    if ((static_cast<uint8_t>(flags) & static_cast<uint8_t>(PageFlags::Execute)) == 0)
    {
        entry |= NoExecute;
    }
    level[pt_index] = entry;
    InvalidatePage(virtual_address);
    return true;
}

bool TranslateUserPage(const AddressSpace& address_space, uint64_t virtual_address,
                       uint64_t* physical_address, bool* writable)
{
    if (virtual_address >= 0x0000800000000000 || address_space.root_physical_address == 0 ||
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
        if ((entry & Present) == 0 || (entry & LargePage) != 0 || (entry & UserAccessible) == 0)
        {
            return false;
        }
        level = PhysicalTable(entry & PageAddressMask);
    }
    if (level == nullptr)
    {
        return false;
    }

    const uint64_t entry = level[(virtual_address >> 12) & 0x1FF];
    if ((entry & Present) == 0 || (entry & UserAccessible) == 0)
    {
        return false;
    }
    *physical_address = (entry & PageAddressMask) | (virtual_address & (PageSize - 1));
    *writable = (entry & Writable) != 0;
    return true;
}

bool UnmapUserPage(AddressSpace& address_space, uint64_t virtual_address,
                   uint64_t* physical_address)
{
    FeatherOS::Sync::SpinLockGuard guard(page_table_lock);
    if (virtual_address >= 0x0000800000000000 || virtual_address % PageSize != 0 ||
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
        if ((entry & Present) == 0 || (entry & LargePage) != 0 || (entry & UserAccessible) == 0)
        {
            return false;
        }
        level = PhysicalTable(entry & PageAddressMask);
    }
    if (level == nullptr)
    {
        return false;
    }

    const size_t pt_index = (virtual_address >> 12) & 0x1FF;
    const uint64_t entry = level[pt_index];
    if ((entry & Present) == 0 || (entry & UserAccessible) == 0)
    {
        return false;
    }
    level[pt_index] = 0;
    // Only this CPU's TLB is flushed. That is enough while a process runs on one CPU at a time,
    // because switching to an address space reloads CR3, which drops its non-global entries.
    InvalidatePage(virtual_address);
    *physical_address = entry & PageAddressMask;
    return true;
}

bool ActivateAddressSpace(const AddressSpace& address_space)
{
    if (address_space.root_physical_address == 0 ||
        (address_space.root_physical_address & ~PageAddressMask) != 0)
    {
        return false;
    }
    const uint64_t root = address_space.root_physical_address;
    asm volatile("mov %0, %%cr3" : : "r"(root) : "memory");
    active_root_physical_addresses[FeatherOS::Cpu::CurrentIndex()] = root;
    return true;
}

void ActivateKernelAddressSpace()
{
    const uint64_t root = kernel_root_physical_address;
    asm volatile("mov %0, %%cr3" : : "r"(root) : "memory");
    active_root_physical_addresses[FeatherOS::Cpu::CurrentIndex()] = root;
}

bool UnmapPage(uint64_t virtual_address)
{
    FeatherOS::Sync::SpinLockGuard guard(page_table_lock);
    if (root_table == nullptr || !IsCanonical(virtual_address) || virtual_address % PageSize != 0)
    {
        return false;
    }

    const size_t pml4_index = (virtual_address >> 39) & 0x1FF;
    const size_t pdpt_index = (virtual_address >> 30) & 0x1FF;
    const size_t pd_index = (virtual_address >> 21) & 0x1FF;
    const size_t pt_index = (virtual_address >> 12) & 0x1FF;
    uint64_t entry = root_table[pml4_index];
    if ((entry & Present) == 0 || (entry & LargePage) != 0)
    {
        return false;
    }
    volatile uint64_t* pdpt = PhysicalTable(entry & PageAddressMask);
    if (pdpt == nullptr)
    {
        return false;
    }
    entry = pdpt[pdpt_index];
    if ((entry & Present) == 0 || (entry & LargePage) != 0)
    {
        return false;
    }
    volatile uint64_t* page_directory = PhysicalTable(entry & PageAddressMask);
    if (page_directory == nullptr)
    {
        return false;
    }
    entry = page_directory[pd_index];
    if ((entry & Present) == 0 || (entry & LargePage) != 0)
    {
        return false;
    }
    volatile uint64_t* page_table = PhysicalTable(entry & PageAddressMask);
    if (page_table == nullptr || (page_table[pt_index] & Present) == 0)
    {
        return false;
    }

    page_table[pt_index] = 0;
    InvalidatePage(virtual_address);
    return true;
}

} // namespace FeatherOS::Memory::Arch::X86_64