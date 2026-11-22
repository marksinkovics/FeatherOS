#include "pci_device_tree.h"

#include <stddef.h>

namespace FeatherOS::Arch::AArch64::PciDeviceTree
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
constexpr uint32_t MaximumControllers = 16;
constexpr uint32_t MaximumMsiFrames = 8;
constexpr uint32_t PciAddressCells = 3;
constexpr uint32_t SpaceIo = 1;
constexpr uint32_t SpaceMemory32 = 2;
constexpr uint32_t SpaceMemory64 = 3;
constexpr uint32_t GicTypeSpi = 0;
constexpr uint32_t GicTypePpi = 1;

struct Property
{
    const uint8_t* value = nullptr;
    uint32_t length = 0;
    bool present = false;
};

struct Controller
{
    uint32_t phandle;
    uint32_t address_cells;
    uint32_t interrupt_cells;
};

struct MsiFrame
{
    uint32_t phandle;
    uint64_t base;
    uint64_t size;
};

struct Node
{
    uint32_t reg_address_cells;
    uint32_t reg_size_cells;
    uint32_t child_address_cells;
    uint32_t child_size_cells;
    uint32_t own_address_cells;
    uint32_t interrupt_cells;
    uint32_t phandle;
    bool host;
    bool disabled;
    Property reg;
    Property bus_range;
    Property ranges;
    Property interrupt_map;
    Property interrupt_map_mask;
    Property msi_parent;
    Property msi_map;
    Property dma_coherent;
};

uint32_t ReadBigEndian32(const uint8_t* data)
{
    return (static_cast<uint32_t>(data[0]) << 24) | (static_cast<uint32_t>(data[1]) << 16) |
           (static_cast<uint32_t>(data[2]) << 8) | data[3];
}

const uint8_t* CellAt(const uint8_t* base, size_t index)
{
    return base + index * 4;
}

uint64_t ReadCells(const uint8_t* data, uint32_t count)
{
    uint64_t value = 0;
    for (uint32_t index = 0; index < count; ++index)
    {
        value = (value << 32) | ReadBigEndian32(data + static_cast<size_t>(index) * 4);
    }
    return value;
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

bool ListContains(const Property& list, const char* wanted)
{
    uint32_t offset = 0;
    while (offset < list.length)
    {
        uint32_t length = 0;
        while (offset + length < list.length && list.value[offset + length] != '\0')
        {
            ++length;
        }
        if (offset + length == list.length)
        {
            return false;
        }
        if (StringEquals(list.value + offset, length + 1, wanted))
        {
            return true;
        }
        offset += length + 1;
    }
    return false;
}

struct Tree
{
    Node nodes[MaxFdtDepth];
    Controller controllers[MaximumControllers];
    uint32_t controller_count = 0;
    MsiFrame frames[MaximumMsiFrames];
    uint32_t frame_count = 0;
    Node host;
    bool host_found = false;
    Property host_compatible;
    Property compatible[MaxFdtDepth];
};

Tree scratch;

void BeginNode(Tree& tree, size_t depth)
{
    Node& node = tree.nodes[depth];
    node = {};
    node.reg_address_cells = depth == 0 ? 2 : tree.nodes[depth - 1].child_address_cells;
    node.reg_size_cells = depth == 0 ? 1 : tree.nodes[depth - 1].child_size_cells;
    // The devicetree defaults when a node does not say.
    node.child_address_cells = 2;
    node.child_size_cells = 1;
    tree.compatible[depth] = {};
}

void SetProperty(Tree& tree, size_t depth, const uint8_t* name, uint32_t name_limit,
                 const uint8_t* value, uint32_t length)
{
    Node& node = tree.nodes[depth];
    const Property property = {value, length, true};
    if (StringEquals(name, name_limit, "compatible"))
    {
        tree.compatible[depth] = property;
    }
    else if (StringEquals(name, name_limit, "reg"))
    {
        node.reg = property;
    }
    else if (StringEquals(name, name_limit, "bus-range"))
    {
        node.bus_range = property;
    }
    else if (StringEquals(name, name_limit, "ranges"))
    {
        node.ranges = property;
    }
    else if (StringEquals(name, name_limit, "interrupt-map"))
    {
        node.interrupt_map = property;
    }
    else if (StringEquals(name, name_limit, "interrupt-map-mask"))
    {
        node.interrupt_map_mask = property;
    }
    else if (StringEquals(name, name_limit, "msi-parent"))
    {
        node.msi_parent = property;
    }
    else if (StringEquals(name, name_limit, "msi-map"))
    {
        node.msi_map = property;
    }
    else if (StringEquals(name, name_limit, "dma-coherent"))
    {
        node.dma_coherent = property;
    }
    else if (StringEquals(name, name_limit, "status"))
    {
        node.disabled = StringEquals(value, length, "disabled");
    }
    else if (length == 4 && StringEquals(name, name_limit, "phandle"))
    {
        node.phandle = ReadBigEndian32(value);
    }
    else if (length == 4 && StringEquals(name, name_limit, "#address-cells"))
    {
        node.child_address_cells = ReadBigEndian32(value);
        node.own_address_cells = node.child_address_cells;
    }
    else if (length == 4 && StringEquals(name, name_limit, "#size-cells"))
    {
        node.child_size_cells = ReadBigEndian32(value);
    }
    else if (length == 4 && StringEquals(name, name_limit, "#interrupt-cells"))
    {
        node.interrupt_cells = ReadBigEndian32(value);
    }
}

void EndNode(Tree& tree, size_t depth)
{
    const Node& node = tree.nodes[depth];
    if (node.phandle != 0 && tree.controller_count < MaximumControllers)
    {
        tree.controllers[tree.controller_count++] = {node.phandle, node.own_address_cells,
                                                     node.interrupt_cells};
    }
    if (node.phandle != 0 && !node.disabled && node.reg.present &&
        tree.frame_count < MaximumMsiFrames &&
        ListContains(tree.compatible[depth], "arm,gic-v2m-frame") && node.reg_address_cells >= 1 &&
        node.reg_address_cells <= 2 && node.reg_size_cells >= 1 && node.reg_size_cells <= 2 &&
        node.reg.length >= (node.reg_address_cells + node.reg_size_cells) * 4)
    {
        tree.frames[tree.frame_count++] = {
            node.phandle, ReadCells(node.reg.value, node.reg_address_cells),
            ReadCells(CellAt(node.reg.value, node.reg_address_cells), node.reg_size_cells)};
    }
    if (!tree.host_found && !node.disabled &&
        ListContains(tree.compatible[depth], "pci-host-ecam-generic"))
    {
        tree.host = node;
        tree.host_found = true;
    }
}

bool WalkTree(const void* device_tree, Tree& tree)
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
            BeginNode(tree, depth);
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
            EndNode(tree, depth - 1);
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
            SetProperty(tree, depth - 1, strings + name_offset, strings_size - name_offset, cursor,
                        length);
            const uint32_t padded_length = (length + 3) & ~3U;
            if (padded_length > static_cast<uint32_t>(structure_end - cursor))
            {
                return false;
            }
            cursor += padded_length;
        }
        else if (token == FdtEnd)
        {
            break;
        }
        else if (token != FdtNoOp)
        {
            return false;
        }
    }
    return tree.host_found;
}

bool ParseRanges(const Node& node, HostBridge* bridge)
{
    const uint32_t entry_cells = PciAddressCells + node.reg_address_cells + node.child_size_cells;
    if (node.child_address_cells != PciAddressCells || node.child_size_cells == 0 ||
        node.child_size_cells > 2 || node.reg_address_cells == 0 || node.reg_address_cells > 2)
    {
        return false;
    }
    for (uint32_t offset = 0; offset + entry_cells * 4 <= node.ranges.length;
         offset += entry_cells * 4)
    {
        const uint8_t* entry = node.ranges.value + offset;
        const uint32_t flags = ReadBigEndian32(entry);
        const uint32_t space = (flags >> 24) & 3U;
        Window window;
        window.pci_base = ReadCells(entry + 4, 2);
        window.cpu_base = ReadCells(entry + 12, node.reg_address_cells);
        window.size = ReadCells(CellAt(entry, 3 + node.reg_address_cells), node.child_size_cells);
        window.prefetchable = (flags & (1U << 30)) != 0;
        window.next = window.pci_base;
        if (window.size == 0)
        {
            continue;
        }
        if (space == SpaceIo && !bridge->has_io)
        {
            bridge->io = window;
            bridge->has_io = true;
        }
        else if (space == SpaceMemory32 &&
                 (!bridge->has_memory32 || (bridge->memory32.prefetchable && !window.prefetchable)))
        {
            bridge->memory32 = window;
            bridge->has_memory32 = true;
        }
        else if (space == SpaceMemory64 && !bridge->has_memory64)
        {
            bridge->memory64 = window;
            bridge->has_memory64 = true;
        }
    }
    return true;
}

bool ParseInterruptMap(const Node& node, const Tree& tree, HostBridge* bridge)
{
    const uint32_t child_cells = PciAddressCells + 1;
    for (uint32_t index = 0; index < child_cells; ++index)
    {
        const bool masked =
            node.interrupt_map_mask.present && node.interrupt_map_mask.length >= (index + 1) * 4;
        bridge->interrupt_mask[index] =
            masked ? ReadBigEndian32(CellAt(node.interrupt_map_mask.value, index)) : 0xFFFFFFFFU;
    }

    uint32_t offset = 0;
    while (offset + (child_cells + 1) * 4 <= node.interrupt_map.length)
    {
        const uint8_t* entry = node.interrupt_map.value + offset;
        const uint32_t phandle = ReadBigEndian32(CellAt(entry, child_cells));
        const Controller* parent = nullptr;
        for (uint32_t index = 0; index < tree.controller_count; ++index)
        {
            if (tree.controllers[index].phandle == phandle)
            {
                parent = &tree.controllers[index];
            }
        }
        if (parent == nullptr || parent->interrupt_cells < 2)
        {
            return false;
        }
        const uint32_t parent_cells = parent->address_cells + parent->interrupt_cells;
        const uint32_t entry_bytes = (child_cells + 1 + parent_cells) * 4;
        if (offset + entry_bytes > node.interrupt_map.length ||
            bridge->interrupt_map_count == MaximumInterruptMapEntries)
        {
            return false;
        }
        const uint8_t* specifier = CellAt(entry, child_cells + 1 + parent->address_cells);
        const uint32_t type = ReadBigEndian32(specifier);
        const uint32_t number = ReadBigEndian32(specifier + 4);
        InterruptMapEntry& result = bridge->interrupt_map[bridge->interrupt_map_count++];
        for (uint32_t index = 0; index < PciAddressCells; ++index)
        {
            result.child_address[index] = ReadBigEndian32(CellAt(entry, index));
        }
        result.pin = ReadBigEndian32(CellAt(entry, PciAddressCells));
        result.interrupt_id =
            type == GicTypeSpi ? 32 + number : (type == GicTypePpi ? 16 + number : 0);
        offset += entry_bytes;
    }
    return true;
}

} // namespace

bool FindHostBridge(const void* device_tree, HostBridge* bridge)
{
    Tree& tree = scratch;
    tree = {};
    if (bridge == nullptr || !WalkTree(device_tree, tree))
    {
        return false;
    }
    const Node& node = tree.host;
    *bridge = {};

    const uint32_t reg_cells = node.reg_address_cells + node.reg_size_cells;
    if (!node.reg.present || node.reg_address_cells == 0 || node.reg_address_cells > 2 ||
        node.reg_size_cells == 0 || node.reg_size_cells > 2 || node.reg.length < reg_cells * 4)
    {
        return false;
    }
    bridge->ecam_base = ReadCells(node.reg.value, node.reg_address_cells);
    bridge->ecam_size =
        ReadCells(CellAt(node.reg.value, node.reg_address_cells), node.reg_size_cells);
    bridge->first_bus = 0;
    bridge->last_bus = 255;
    if (node.bus_range.present && node.bus_range.length >= 8)
    {
        bridge->first_bus = static_cast<uint8_t>(ReadBigEndian32(node.bus_range.value));
        bridge->last_bus = static_cast<uint8_t>(ReadBigEndian32(node.bus_range.value + 4));
    }
    const uint64_t buses = bridge->ecam_size >> 20;
    if (buses == 0 || bridge->last_bus < bridge->first_bus)
    {
        return false;
    }
    if (static_cast<uint64_t>(bridge->last_bus) - bridge->first_bus + 1 > buses)
    {
        bridge->last_bus = static_cast<uint8_t>(bridge->first_bus + buses - 1);
    }
    bridge->dma_coherent = node.dma_coherent.present;
    // GICv2m ignores the requester ID, so the first msi-map entry (rid-base, phandle, msi-base,
    // length) stands for the whole bus.
    bridge->msi_phandle = 0;
    if (node.msi_parent.present && node.msi_parent.length >= 4)
    {
        bridge->msi_phandle = ReadBigEndian32(node.msi_parent.value);
    }
    else if (node.msi_map.present && node.msi_map.length >= 16)
    {
        bridge->msi_phandle = ReadBigEndian32(CellAt(node.msi_map.value, 1));
    }
    for (uint32_t index = 0; index < tree.frame_count; ++index)
    {
        if (bridge->msi_phandle != 0 && tree.frames[index].phandle == bridge->msi_phandle)
        {
            bridge->has_msi_frame = true;
            bridge->msi_frame_base = tree.frames[index].base;
            bridge->msi_frame_size = tree.frames[index].size;
        }
    }
    return ParseRanges(node, bridge) && ParseInterruptMap(node, tree, bridge);
}

bool LookupInterrupt(const HostBridge& bridge, uint8_t bus, uint8_t device, uint8_t function,
                     uint8_t pin, uint32_t* interrupt_id)
{
    const uint32_t address[3] = {(static_cast<uint32_t>(bus) << 16) |
                                     (static_cast<uint32_t>(device) << 11) |
                                     (static_cast<uint32_t>(function) << 8),
                                 0, 0};
    for (uint32_t index = 0; index < bridge.interrupt_map_count; ++index)
    {
        const InterruptMapEntry& entry = bridge.interrupt_map[index];
        bool match = ((pin ^ entry.pin) & bridge.interrupt_mask[3]) == 0;
        for (uint32_t cell = 0; cell < 3; ++cell)
        {
            match = match && ((address[cell] ^ entry.child_address[cell]) &
                              bridge.interrupt_mask[cell]) == 0;
        }
        if (match && entry.interrupt_id != 0)
        {
            *interrupt_id = entry.interrupt_id;
            return true;
        }
    }
    return false;
}

bool AllocateFromWindow(Window& window, uint64_t size, uint64_t* pci_address)
{
    if (size == 0 || (size & (size - 1)) != 0)
    {
        return false;
    }
    const uint64_t end = window.pci_base + window.size;
    const uint64_t aligned = (window.next + size - 1) & ~(size - 1);
    if (aligned < window.next || aligned > end || size > end - aligned)
    {
        return false;
    }
    window.next = aligned + size;
    *pci_address = aligned;
    return true;
}

} // namespace FeatherOS::Arch::AArch64::PciDeviceTree
