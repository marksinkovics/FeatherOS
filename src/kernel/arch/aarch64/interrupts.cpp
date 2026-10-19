#include "interrupts.h"

#include "kernel/arch/interrupts.h"
#include "kernel/cpu.h"
#include "kernel/panic.h"

#include <stddef.h>
#include <stdint.h>

namespace FeatherOS::Arch::AArch64
{
namespace
{

constexpr uint32_t FdtMagic = 0xD00DFEED;
constexpr uint32_t FdtBeginNode = 1;
constexpr uint32_t FdtEndNode = 2;
constexpr uint32_t FdtProperty = 3;
constexpr uint32_t FdtNoOp = 4;
constexpr uint32_t FdtEnd = 9;
constexpr size_t MaxFdtDepth = 24;
constexpr uint32_t GicSoftwareInterruptCount = 16;
constexpr uint32_t GicArchitectedMaximumInterruptCount = 1020;

constexpr uint32_t GicDistributorControl = 0x000;
constexpr uint32_t GicDistributorType = 0x004;
constexpr uint32_t GicDistributorGroup = 0x080;
constexpr uint32_t GicDistributorEnable = 0x100;
constexpr uint32_t GicDistributorPriority = 0x0400;
constexpr uint32_t GicDistributorTargets = 0x0800;
constexpr uint32_t GicSoftwareInterrupt = 0x0F00;
constexpr uint32_t GicSoftwareInterruptSelf = 2U << 24;
constexpr uint32_t IpiInterruptId = 1;
constexpr uint32_t GicCpuControl = 0x000;
constexpr uint32_t GicCpuPriorityMask = 0x004;
constexpr uint32_t GicCpuInterruptAcknowledge = 0x00C;
constexpr uint32_t GicCpuEndOfInterrupt = 0x010;
constexpr uint32_t GicFirstSpecialInterrupt = 1020;

struct FdtNode
{
    uint32_t reg_address_cells;
    uint32_t reg_size_cells;
    uint32_t child_address_cells;
    uint32_t child_size_cells;
    const uint8_t* compatible;
    uint32_t compatible_length;
    const uint8_t* reg;
    uint32_t reg_length;
    bool interrupt_controller;
    bool disabled;
};

struct GicV2Registers
{
    uintptr_t distributor;
    uintptr_t cpu_interface;
};

InterruptHandler handlers[GicArchitectedMaximumInterruptCount] = {};
uintptr_t distributor_base = 0;
uintptr_t cpu_interface_base = 0;
uint32_t supported_interrupt_count = 0;
bool use_group1 = false;
uint32_t cpu_target_masks[Cpu::MaximumCpus] = {};

uint32_t ReadBigEndian32(const uint8_t* data)
{
    return (static_cast<uint32_t>(data[0]) << 24) | (static_cast<uint32_t>(data[1]) << 16) |
           (static_cast<uint32_t>(data[2]) << 8) | data[3];
}

bool StringEquals(const uint8_t* data, uint32_t length, const char* value)
{
    uint32_t index = 0;
    while (value[index] != '\0')
    {
        if (index >= length || data[index] != static_cast<uint8_t>(value[index]))
        {
            return false;
        }
        ++index;
    }
    return index < length && data[index] == '\0';
}

bool HasCompatible(const FdtNode& node, const char* compatible)
{
    uint32_t offset = 0;
    while (offset < node.compatible_length)
    {
        const uint8_t* value = node.compatible + offset;
        uint32_t length = 0;
        while (offset + length < node.compatible_length && value[length] != '\0')
        {
            ++length;
        }
        if (offset + length == node.compatible_length)
        {
            return false;
        }
        if (StringEquals(value, length + 1, compatible))
        {
            return true;
        }
        offset += length + 1;
    }
    return false;
}

bool ReadCells(const uint8_t* data, uint32_t count, uint64_t* value)
{
    if (count == 0 || count > 2)
    {
        return false;
    }
    uint64_t result = 0;
    for (uint32_t index = 0; index < count; ++index)
    {
        result = (result << 32) | ReadBigEndian32(data + index * 4);
    }
    *value = result;
    return true;
}

bool ReadGicV2Registers(const FdtNode& node, GicV2Registers* registers)
{
    if (node.reg == nullptr || node.reg_address_cells == 0 || node.reg_address_cells > 2 ||
        node.reg_size_cells > 2)
    {
        return false;
    }
    const uint32_t tuple_cells = node.reg_address_cells + node.reg_size_cells;
    const uint32_t tuple_size = tuple_cells * 4;
    if (tuple_size == 0 || node.reg_length < tuple_size * 2)
    {
        return false;
    }

    uint64_t distributor;
    uint64_t cpu_interface;
    if (!ReadCells(node.reg, node.reg_address_cells, &distributor) ||
        !ReadCells(node.reg + tuple_size, node.reg_address_cells, &cpu_interface) ||
        distributor > UINTPTR_MAX || cpu_interface > UINTPTR_MAX)
    {
        return false;
    }
    registers->distributor = static_cast<uintptr_t>(distributor);
    registers->cpu_interface = static_cast<uintptr_t>(cpu_interface);
    return true;
}

bool FindGicV2(const void* device_tree, GicV2Registers* registers)
{
    const auto* bytes = static_cast<const uint8_t*>(device_tree);
    if (bytes == nullptr || ReadBigEndian32(bytes) != FdtMagic)
    {
        return false;
    }

    const uint32_t total_size = ReadBigEndian32(bytes + 4);
    const uint32_t structure_offset = ReadBigEndian32(bytes + 8);
    const uint32_t strings_offset = ReadBigEndian32(bytes + 12);
    const uint32_t strings_size = ReadBigEndian32(bytes + 32);
    const uint32_t structure_size = ReadBigEndian32(bytes + 36);
    if (total_size < 40 || structure_offset > total_size ||
        structure_size > total_size - structure_offset || strings_offset > total_size ||
        strings_size > total_size - strings_offset)
    {
        return false;
    }

    const uint8_t* cursor = bytes + structure_offset;
    const uint8_t* structure_end = cursor + structure_size;
    const uint8_t* strings = bytes + strings_offset;
    static FdtNode nodes[MaxFdtDepth];
    size_t depth = 0;

    while (cursor + 4 <= structure_end)
    {
        const uint32_t token = ReadBigEndian32(cursor);
        cursor += 4;
        if (token == FdtBeginNode)
        {
            if (depth == MaxFdtDepth)
            {
                return false;
            }
            FdtNode& node = nodes[depth];
            node.compatible = nullptr;
            node.compatible_length = 0;
            node.reg = nullptr;
            node.reg_length = 0;
            node.interrupt_controller = false;
            node.disabled = false;
            if (depth == 0)
            {
                node.reg_address_cells = 2;
                node.reg_size_cells = 1;
                node.child_address_cells = 2;
                node.child_size_cells = 1;
            }
            else
            {
                node.reg_address_cells = nodes[depth - 1].child_address_cells;
                node.reg_size_cells = nodes[depth - 1].child_size_cells;
                node.child_address_cells = node.reg_address_cells;
                node.child_size_cells = node.reg_size_cells;
            }
            ++depth;
            while (cursor < structure_end && *cursor != '\0')
            {
                ++cursor;
            }
            if (cursor == structure_end)
            {
                return false;
            }
            ++cursor;
            cursor += (4 - (reinterpret_cast<uintptr_t>(cursor) & 3)) & 3;
        }
        else if (token == FdtEndNode)
        {
            if (depth == 0)
            {
                return false;
            }
            const FdtNode& node = nodes[depth - 1];
            if (node.interrupt_controller && !node.disabled &&
                (HasCompatible(node, "arm,cortex-a15-gic") || HasCompatible(node, "arm,gic-400")))
            {
                return ReadGicV2Registers(node, registers);
            }
            --depth;
        }
        else if (token == FdtProperty)
        {
            if (depth == 0 || cursor + 8 > structure_end)
            {
                return false;
            }
            const uint32_t length = ReadBigEndian32(cursor);
            const uint32_t name_offset = ReadBigEndian32(cursor + 4);
            cursor += 8;
            if (length > static_cast<uint32_t>(structure_end - cursor) ||
                name_offset >= strings_size)
            {
                return false;
            }

            const char* name = reinterpret_cast<const char*>(strings + name_offset);
            FdtNode& node = nodes[depth - 1];
            if (StringEquals(reinterpret_cast<const uint8_t*>(name), strings_size - name_offset,
                             "compatible"))
            {
                node.compatible = cursor;
                node.compatible_length = length;
            }
            else if (StringEquals(reinterpret_cast<const uint8_t*>(name),
                                  strings_size - name_offset, "reg"))
            {
                node.reg = cursor;
                node.reg_length = length;
            }
            else if (StringEquals(reinterpret_cast<const uint8_t*>(name),
                                  strings_size - name_offset, "interrupt-controller"))
            {
                node.interrupt_controller = true;
            }
            else if (StringEquals(reinterpret_cast<const uint8_t*>(name),
                                  strings_size - name_offset, "status"))
            {
                node.disabled = StringEquals(cursor, length, "disabled");
            }
            else if (StringEquals(reinterpret_cast<const uint8_t*>(name),
                                  strings_size - name_offset, "#address-cells") &&
                     length == 4)
            {
                node.child_address_cells = ReadBigEndian32(cursor);
            }
            else if (StringEquals(reinterpret_cast<const uint8_t*>(name),
                                  strings_size - name_offset, "#size-cells") &&
                     length == 4)
            {
                node.child_size_cells = ReadBigEndian32(cursor);
            }

            const uint32_t padded_length = (length + 3) & ~3U;
            if (padded_length > static_cast<uint32_t>(structure_end - cursor))
            {
                return false;
            }
            cursor += padded_length;
        }
        else if (token == FdtNoOp)
        {
            continue;
        }
        else if (token == FdtEnd)
        {
            return false;
        }
        else
        {
            return false;
        }
    }
    return false;
}

volatile uint32_t& DistributorRegister(uint32_t offset)
{
    return *reinterpret_cast<volatile uint32_t*>(distributor_base + offset);
}

volatile uint32_t& CpuRegister(uint32_t offset)
{
    return *reinterpret_cast<volatile uint32_t*>(cpu_interface_base + offset);
}

void ConfigureInterrupt(uint32_t interrupt_id)
{
    const uint32_t register_group = interrupt_id / 32;
    const uint32_t bit = 1U << (interrupt_id % 32);
    volatile uint32_t& group = DistributorRegister(GicDistributorGroup + register_group * 4);
    if (use_group1)
    {
        group |= bit;
    }
    else
    {
        group &= ~bit;
    }

    const uint32_t priority_offset = GicDistributorPriority + (interrupt_id / 4) * 4;
    const uint32_t priority_shift = (interrupt_id % 4) * 8;
    volatile uint32_t& priority = DistributorRegister(priority_offset);
    priority = (priority & ~(0xFFU << priority_shift)) | (0xA0U << priority_shift);

    DistributorRegister(GicDistributorEnable + register_group * 4) = bit;
}

void EnableCpuInterface()
{
    CpuRegister(GicCpuPriorityMask) = 0xFF;
    CpuRegister(GicCpuControl) = use_group1 ? 3 : 1;
    // The banked target register for SGI 0 reads back this CPU's own target bit.
    cpu_target_masks[Cpu::CurrentIndex()] = DistributorRegister(GicDistributorTargets) & 0xFF;
}

} // namespace

bool InitializeInterruptController(const void* device_tree)
{
    if (distributor_base != 0)
    {
        return true;
    }

    GicV2Registers registers;
    registers.distributor = 0;
    registers.cpu_interface = 0;
    if (!FindGicV2(device_tree, &registers))
    {
        return false;
    }
    distributor_base = registers.distributor;
    cpu_interface_base = registers.cpu_interface;

    asm volatile("msr daifset, #2" ::: "memory");
    DistributorRegister(GicDistributorControl) = 0;
    CpuRegister(GicCpuControl) = 0;
    const uint32_t distributor_type = DistributorRegister(GicDistributorType);
    const uint32_t implemented_interrupts = 32 * ((distributor_type & 0x1F) + 1);
    supported_interrupt_count = implemented_interrupts < GicArchitectedMaximumInterruptCount
                                    ? implemented_interrupts
                                    : GicArchitectedMaximumInterruptCount;
    use_group1 = (distributor_type & (1U << 10)) != 0;
    if (use_group1)
    {
        DistributorRegister(GicDistributorGroup) |= 1;
    }
    else
    {
        DistributorRegister(GicDistributorGroup) &= ~1U;
    }
    DistributorRegister(GicDistributorControl) = use_group1 ? 3 : 1;
    EnableCpuInterface();
    for (uint32_t interrupt_id = 0; interrupt_id < supported_interrupt_count; ++interrupt_id)
    {
        if (handlers[interrupt_id] != nullptr)
        {
            ConfigureInterrupt(interrupt_id);
        }
    }
    asm volatile("dsb sy\n\tisb" ::: "memory");
    return true;
}

void RegisterInterruptHandler(uint32_t interrupt_id, InterruptHandler handler)
{
    if (interrupt_id >= GicArchitectedMaximumInterruptCount || handler == nullptr)
    {
        Panic("Invalid AArch64 interrupt handler registration");
    }
    handlers[interrupt_id] = handler;
    if (distributor_base != 0 && interrupt_id < supported_interrupt_count)
    {
        ConfigureInterrupt(interrupt_id);
    }
}

void TriggerSoftwareInterrupt(uint8_t interrupt_id)
{
    if (interrupt_id >= GicSoftwareInterruptCount || distributor_base == 0)
    {
        Panic("Invalid AArch64 software interrupt request");
    }
    DistributorRegister(GicSoftwareInterrupt) = GicSoftwareInterruptSelf | interrupt_id;
    asm volatile("dsb sy" ::: "memory");
}

extern "C" void FeatherOS_AArch64_IrqDispatch()
{
    const uint32_t acknowledge = CpuRegister(GicCpuInterruptAcknowledge);
    const uint32_t interrupt_id = acknowledge & 0x3FF;
    if (interrupt_id >= GicFirstSpecialInterrupt)
    {
        return;
    }
    CpuRegister(GicCpuEndOfInterrupt) = acknowledge;
    if (interrupt_id < supported_interrupt_count && handlers[interrupt_id] != nullptr)
    {
        handlers[interrupt_id]();
    }
}

} // namespace FeatherOS::Arch::AArch64

namespace FeatherOS::Arch::Interrupts
{

uint64_t SaveAndDisable()
{
    uint64_t flags;
    asm volatile("mrs %0, daif; msr daifset, #2" : "=r"(flags) : : "memory");
    return flags;
}

void Restore(uint64_t flags)
{
    if ((flags & (1U << 7)) == 0)
    {
        asm volatile("msr daifclr, #2" : : : "memory");
    }
}

void Enable()
{
    asm volatile("msr daifclr, #2" : : : "memory");
}

void InitializeSecondaryCpu()
{
    // SGI and PPI configuration is banked per CPU, so each CPU enables the ones with handlers.
    for (uint32_t interrupt_id = 0; interrupt_id < 32; ++interrupt_id)
    {
        if (AArch64::handlers[interrupt_id] != nullptr)
        {
            AArch64::ConfigureInterrupt(interrupt_id);
        }
    }
    AArch64::EnableCpuInterface();
    asm volatile("dsb sy\n\tisb" ::: "memory");
}

void SetIpiHandler(IpiHandler handler)
{
    AArch64::RegisterInterruptHandler(AArch64::IpiInterruptId, handler);
}

void SendIpi(uint32_t cpu_index)
{
    if (cpu_index >= Cpu::MaximumCpus || AArch64::cpu_target_masks[cpu_index] == 0)
    {
        Panic("IPI target CPU has no GIC interface");
    }
    asm volatile("dsb ishst" ::: "memory");
    AArch64::DistributorRegister(AArch64::GicSoftwareInterrupt) =
        (AArch64::cpu_target_masks[cpu_index] << 16) | AArch64::IpiInterruptId;
}

void WaitForInterrupt()
{
    // WFI wakes on a pending interrupt even while IRQs are masked; unmasking then takes it.
    asm volatile("wfi\n\t"
                 "msr daifclr, #2\n\t"
                 "isb"
                 :
                 :
                 : "memory");
}

} // namespace FeatherOS::Arch::Interrupts