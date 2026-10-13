#include "virtual_memory.h"

#include "kernel/memory/page_allocator.h"

#include <stddef.h>

namespace FeatherOS::Memory::Arch::X86_64
{
namespace
{

constexpr uint64_t PageAddressMask = 0x000FFFFFFFFFF000;
constexpr uint64_t Present = 1;
constexpr uint64_t Writable = 1U << 1;
constexpr uint64_t LargePage = 1U << 7;
constexpr uint64_t NoExecute = 1ULL << 63;
constexpr uint64_t EferMsr = 0xC0000080;
constexpr uint64_t EferNoExecuteEnable = 1U << 11;

uint64_t hhdm_offset = 0;
uint64_t* root_table = nullptr;

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

bool GetOrCreateTable(volatile uint64_t* parent, size_t index, volatile uint64_t** child)
{
    uint64_t entry = parent[index];
    if ((entry & Present) != 0)
    {
        if ((entry & LargePage) != 0)
        {
            return false;
        }
        parent[index] = entry | Present | Writable;
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
    parent[index] = (physical_address & PageAddressMask) | Present | Writable;
    *child = table;
    return true;
}

void InvalidatePage(uint64_t virtual_address)
{
    asm volatile("invlpg (%0)" : : "r"(virtual_address) : "memory");
}

} // namespace

bool InitializeVirtualMemory(uint64_t offset)
{
    hhdm_offset = offset;
    uint64_t cr3;
    asm volatile("mov %%cr3, %0" : "=r"(cr3));
    root_table = const_cast<uint64_t*>(PhysicalTable(cr3 & PageAddressMask));
    if (root_table == nullptr)
    {
        return false;
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
    if (root_table == nullptr || !IsCanonical(virtual_address) || virtual_address % PageSize != 0 ||
        physical_address % PageSize != 0 || (physical_address & ~PageAddressMask) != 0)
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
    if (!GetOrCreateTable(root_table, pml4_index, &pdpt) ||
        !GetOrCreateTable(pdpt, pdpt_index, &page_directory) ||
        !GetOrCreateTable(page_directory, pd_index, &page_table))
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
    if ((static_cast<uint8_t>(flags) & static_cast<uint8_t>(PageFlags::Execute)) == 0)
    {
        entry |= NoExecute;
    }
    page_table[pt_index] = entry;
    InvalidatePage(virtual_address);
    return true;
}

bool UnmapPage(uint64_t virtual_address)
{
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