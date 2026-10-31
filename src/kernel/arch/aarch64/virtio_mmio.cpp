#include "virtio_mmio.h"

#include "kernel/memory/device_memory.h"

#include <stddef.h>
#include <stdint.h>

namespace FeatherOS::Arch::AArch64::VirtioMmio
{
namespace
{

constexpr uint32_t FdtMagic = 0xD00DFEED;
constexpr uint32_t FdtBeginNode = 1;
constexpr uint32_t FdtEndNode = 2;
constexpr uint32_t FdtProperty = 3;
constexpr uint32_t FdtNoOp = 4;
constexpr size_t MaxFdtDepth = 24;
constexpr uint64_t RegisterSize = 0x200;
constexpr uint32_t VirtioMagic = 0x74726976;

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
    const uint8_t* interrupts;
    uint32_t interrupts_length;
    bool disabled;
};

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

bool MapNode(const FdtNode& node, uint32_t device_id, Device* device)
{
    if (node.disabled || node.reg == nullptr || !HasCompatible(node, "virtio,mmio"))
    {
        return false;
    }
    const uint32_t tuple_cells = node.reg_address_cells + node.reg_size_cells;
    uint64_t address;
    if (tuple_cells == 0 || node.reg_length < tuple_cells * 4 ||
        !ReadCells(node.reg, node.reg_address_cells, &address) || address > UINTPTR_MAX)
    {
        return false;
    }

    uint32_t interrupt_id = 0;
    if (node.interrupts != nullptr && node.interrupts_length >= 12)
    {
        // GIC interrupt specifiers: type 0 is a shared peripheral interrupt, 1 a private one.
        const uint32_t interrupt_type = ReadBigEndian32(node.interrupts);
        const uint32_t interrupt_number = ReadBigEndian32(node.interrupts + 4);
        if (interrupt_type == 0)
        {
            interrupt_id = 32 + interrupt_number;
        }
        else if (interrupt_type == 1)
        {
            interrupt_id = 16 + interrupt_number;
        }
    }

    const uintptr_t mapped = Memory::MapDevice(address, RegisterSize);
    const auto* registers = reinterpret_cast<volatile uint32_t*>(mapped);
    if (mapped == 0 || registers[0] != VirtioMagic || registers[1] < 2 || registers[2] != device_id)
    {
        return false;
    }
    device->base = mapped;
    device->interrupt_id = interrupt_id;
    return true;
}

} // namespace

bool Find(const void* device_tree, uint32_t device_id, Device* device)
{
    const auto* bytes = static_cast<const uint8_t*>(device_tree);
    if (bytes == nullptr || device == nullptr || ReadBigEndian32(bytes) != FdtMagic)
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
    FdtNode nodes[MaxFdtDepth];
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
            node.reg_address_cells = depth == 0 ? 2 : nodes[depth - 1].child_address_cells;
            node.reg_size_cells = depth == 0 ? 2 : nodes[depth - 1].child_size_cells;
            node.child_address_cells = node.reg_address_cells;
            node.child_size_cells = node.reg_size_cells;
            node.compatible = nullptr;
            node.compatible_length = 0;
            node.reg = nullptr;
            node.reg_length = 0;
            node.interrupts = nullptr;
            node.interrupts_length = 0;
            node.disabled = false;
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
            if (MapNode(nodes[depth - 1], device_id, device))
            {
                return true;
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

            FdtNode& node = nodes[depth - 1];
            const uint8_t* value = cursor;
            const auto* name = strings + name_offset;
            const uint32_t name_limit = strings_size - name_offset;
            if (StringEquals(name, name_limit, "compatible"))
            {
                node.compatible = value;
                node.compatible_length = length;
            }
            else if (StringEquals(name, name_limit, "reg"))
            {
                node.reg = value;
                node.reg_length = length;
            }
            else if (StringEquals(name, name_limit, "interrupts"))
            {
                node.interrupts = value;
                node.interrupts_length = length;
            }
            else if (StringEquals(name, name_limit, "status"))
            {
                node.disabled = StringEquals(value, length, "disabled");
            }
            else if (StringEquals(name, name_limit, "#address-cells") && length == 4)
            {
                node.child_address_cells = ReadBigEndian32(value);
            }
            else if (StringEquals(name, name_limit, "#size-cells") && length == 4)
            {
                node.child_size_cells = ReadBigEndian32(value);
            }

            const uint32_t padded_length = (length + 3) & ~3U;
            if (padded_length > static_cast<uint32_t>(structure_end - cursor))
            {
                return false;
            }
            cursor += padded_length;
        }
        else if (token != FdtNoOp)
        {
            return false;
        }
    }
    return false;
}

} // namespace FeatherOS::Arch::AArch64::VirtioMmio
