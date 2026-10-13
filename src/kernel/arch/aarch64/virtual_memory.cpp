#include "virtual_memory.h"

#include "kernel/memory/page_allocator.h"

#include <stddef.h>

namespace FeatherOS::Memory::Arch::AArch64
{
namespace
{

constexpr uint64_t PageAddressMask = 0x0000FFFFFFFFF000;
constexpr uint64_t MairNormalWriteBack = 0xFF;
constexpr uint64_t DescriptorValid = 1;
constexpr uint64_t DescriptorTableOrPage = 1U << 1;
constexpr uint64_t AccessFlag = 1U << 10;
constexpr uint64_t InnerShareable = 3U << 8;
constexpr uint64_t AccessPermissionReadOnly = 1U << 7;
constexpr uint64_t PrivilegedExecuteNever = 1ULL << 53;
constexpr uint64_t UnprivilegedExecuteNever = 1ULL << 54;

uint64_t hhdm_offset = 0;
volatile uint64_t* root_table = nullptr;
uint8_t normal_memory_attribute_index = 0;

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

void InvalidatePage(uint64_t virtual_address)
{
    const uint64_t operand = virtual_address >> 12;
    asm volatile("dsb ishst\n\ttlbi vaae1is, %0\n\tdsb ish\n\tisb" : : "r"(operand) : "memory");
}

} // namespace

bool InitializeVirtualMemory(uint64_t offset)
{
    hhdm_offset = offset;
    uint64_t mair;
    asm volatile("mrs %0, MAIR_EL1" : "=r"(mair));
    bool found_normal_memory_attribute = false;
    for (uint8_t index = 0; index < 8; ++index)
    {
        if (((mair >> (index * 8)) & 0xFF) == MairNormalWriteBack)
        {
            normal_memory_attribute_index = index;
            found_normal_memory_attribute = true;
            break;
        }
    }
    if (!found_normal_memory_attribute)
    {
        return false;
    }

    uint64_t ttbr1;
    asm volatile("mrs %0, TTBR1_EL1" : "=r"(ttbr1));
    root_table = PhysicalTable(ttbr1 & PageAddressMask);
    return root_table != nullptr;
}

bool MapPage(uint64_t virtual_address, uint64_t physical_address, PageFlags flags)
{
    if (root_table == nullptr || !IsCanonical(virtual_address) || virtual_address % PageSize != 0 ||
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
    if (!GetOrCreateTable(root_table, level0_index, &level1) ||
        !GetOrCreateTable(level1, level1_index, &level2) ||
        !GetOrCreateTable(level2, level2_index, &level3))
    {
        return false;
    }
    if ((level3[level3_index] & DescriptorValid) != 0)
    {
        return false;
    }

    uint64_t descriptor = (physical_address & PageAddressMask) | DescriptorValid |
                          DescriptorTableOrPage | AccessFlag | InnerShareable |
                          (static_cast<uint64_t>(normal_memory_attribute_index) << 2);
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

bool UnmapPage(uint64_t virtual_address)
{
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
