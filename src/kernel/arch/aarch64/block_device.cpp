#include "kernel/storage/block_device.h"

#include "kernel/memory/page_allocator.h"

#include <stddef.h>
#include <stdint.h>

namespace FeatherOS::BlockDevice
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

constexpr uint32_t VirtioMagic = 0x74726976;
constexpr uint32_t VirtioBlockDeviceId = 2;
constexpr uint32_t VirtioVersion = 0x004;
constexpr uint32_t VirtioDeviceId = 0x008;
constexpr uint32_t VirtioDeviceFeatures = 0x010;
constexpr uint32_t VirtioDeviceFeaturesSelect = 0x014;
constexpr uint32_t VirtioDriverFeatures = 0x020;
constexpr uint32_t VirtioDriverFeaturesSelect = 0x024;
constexpr uint32_t VirtioQueueSelect = 0x030;
constexpr uint32_t VirtioQueueSizeMaximum = 0x034;
constexpr uint32_t VirtioQueueSize = 0x038;
constexpr uint32_t VirtioQueueReady = 0x044;
constexpr uint32_t VirtioQueueNotify = 0x050;
constexpr uint32_t VirtioDeviceStatus = 0x070;
constexpr uint32_t VirtioQueueDescriptorLow = 0x080;
constexpr uint32_t VirtioQueueAvailableLow = 0x090;
constexpr uint32_t VirtioQueueUsedLow = 0x0A0;
constexpr uint32_t VirtioConfig = 0x100;

constexpr uint32_t StatusAcknowledge = 1;
constexpr uint32_t StatusDriver = 2;
constexpr uint32_t StatusDriverOk = 4;
constexpr uint32_t StatusFeaturesOk = 8;
constexpr uint32_t StatusFailed = 128;
constexpr uint32_t FeatureVersionOne = 1;
constexpr uint32_t QueueSize = 8;
constexpr uint32_t DescriptorOffset = 0;
constexpr uint32_t AvailableOffset = sizeof(uint64_t) * 2 * QueueSize;
constexpr uint32_t UsedOffset = (AvailableOffset + 4 + 2 * QueueSize + 3) & ~3U;
constexpr uint32_t RequestOffset = (UsedOffset + 4 + 8 * QueueSize + 7) & ~7U;
constexpr uint32_t DataOffset = RequestOffset + 16;
constexpr uint32_t StatusOffset = DataOffset + SectorSize;
constexpr uint16_t DescriptorNext = 1;
constexpr uint16_t DescriptorDeviceWrite = 2;
constexpr uint32_t RequestRead = 0;
constexpr uint8_t RequestStatusPending = 0xFF;

struct FdtNode
{
    uint32_t address_cells;
    uint32_t size_cells;
    uint32_t child_address_cells;
    uint32_t child_size_cells;
    const uint8_t* compatible;
    uint32_t compatible_length;
    const uint8_t* reg;
    uint32_t reg_length;
};

struct __attribute__((packed)) Descriptor
{
    uint64_t address;
    uint32_t length;
    uint16_t flags;
    uint16_t next;
};

struct __attribute__((packed)) UsedElement
{
    uint32_t id;
    uint32_t length;
};

struct BlockRequest
{
    uint32_t type;
    uint32_t reserved;
    uint64_t sector;
};

uintptr_t device_base = 0;
uint64_t device_hhdm_offset = 0;
uint64_t queue_physical_address = 0;
volatile uint8_t* queue_virtual_address = nullptr;
uint64_t capacity_sectors = 0;
uint16_t available_index_value = 0;
bool initialized = false;

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

bool FindDevice(const void* device_tree, uintptr_t* base)
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
            node.address_cells = depth == 0 ? 2 : nodes[depth - 1].child_address_cells;
            node.size_cells = depth == 0 ? 2 : nodes[depth - 1].child_size_cells;
            node.child_address_cells = node.address_cells;
            node.child_size_cells = node.size_cells;
            node.compatible = nullptr;
            node.compatible_length = 0;
            node.reg = nullptr;
            node.reg_length = 0;
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
            if (node.reg != nullptr && HasCompatible(node, "virtio,mmio"))
            {
                const uint32_t tuple_cells = node.address_cells + node.size_cells;
                uint64_t address;
                if (tuple_cells != 0 && node.reg_length >= tuple_cells * 4 &&
                    ReadCells(node.reg, node.address_cells, &address) && address <= UINTPTR_MAX)
                {
                    auto* registers =
                        reinterpret_cast<volatile uint32_t*>(static_cast<uintptr_t>(address));
                    if (registers[0] == VirtioMagic && registers[1] >= 2 &&
                        registers[2] == VirtioBlockDeviceId)
                    {
                        *base = static_cast<uintptr_t>(address);
                        return true;
                    }
                }
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
            const char* name = reinterpret_cast<const char*>(strings + name_offset);
            if (StringEquals(reinterpret_cast<const uint8_t*>(name), strings_size - name_offset,
                             "compatible"))
            {
                node.compatible = value;
                node.compatible_length = length;
            }
            else if (StringEquals(reinterpret_cast<const uint8_t*>(name),
                                  strings_size - name_offset, "reg"))
            {
                node.reg = value;
                node.reg_length = length;
            }
            else if (StringEquals(reinterpret_cast<const uint8_t*>(name),
                                  strings_size - name_offset, "#address-cells") &&
                     length == 4)
            {
                node.child_address_cells = ReadBigEndian32(value);
            }
            else if (StringEquals(reinterpret_cast<const uint8_t*>(name),
                                  strings_size - name_offset, "#size-cells") &&
                     length == 4)
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

volatile uint32_t* Registers()
{
    return reinterpret_cast<volatile uint32_t*>(device_base);
}

void WriteQueueAddress(uint32_t offset, uint64_t address)
{
    Registers()[offset / 4] = static_cast<uint32_t>(address);
    Registers()[(offset + 4) / 4] = static_cast<uint32_t>(address >> 32);
}

bool InitializeQueue()
{
    volatile uint32_t* registers = Registers();
    registers[VirtioQueueSelect / 4] = 0;
    if (registers[VirtioQueueSizeMaximum / 4] < QueueSize ||
        !Memory::AllocatePageFrame(&queue_physical_address) ||
        queue_physical_address > UINT64_MAX - device_hhdm_offset)
    {
        return false;
    }

    queue_virtual_address = reinterpret_cast<volatile uint8_t*>(
        static_cast<uintptr_t>(device_hhdm_offset + queue_physical_address));
    for (uint32_t index = 0; index < Memory::PageSize; ++index)
    {
        queue_virtual_address[index] = 0;
    }

    auto* descriptors = reinterpret_cast<volatile Descriptor*>(queue_virtual_address);
    auto* available_index =
        reinterpret_cast<volatile uint16_t*>(queue_virtual_address + AvailableOffset + 2);
    auto* available_ring =
        reinterpret_cast<volatile uint16_t*>(queue_virtual_address + AvailableOffset + 4);
    auto* request = reinterpret_cast<volatile BlockRequest*>(queue_virtual_address + RequestOffset);
    auto* status = queue_virtual_address + StatusOffset;

    descriptors[0].address = queue_physical_address + RequestOffset;
    descriptors[0].length = sizeof(BlockRequest);
    descriptors[0].flags = DescriptorNext;
    descriptors[0].next = 1;
    descriptors[1].address = queue_physical_address + DataOffset;
    descriptors[1].length = SectorSize;
    descriptors[1].flags = DescriptorNext | DescriptorDeviceWrite;
    descriptors[1].next = 2;
    descriptors[2].address = queue_physical_address + StatusOffset;
    descriptors[2].length = 1;
    descriptors[2].flags = DescriptorDeviceWrite;
    descriptors[2].next = 0;
    available_ring[0] = 0;
    *available_index = 0;
    request->type = RequestRead;
    request->reserved = 0;
    request->sector = 0;
    *status = RequestStatusPending;

    registers[VirtioQueueSize / 4] = QueueSize;
    WriteQueueAddress(VirtioQueueDescriptorLow, queue_physical_address + DescriptorOffset);
    WriteQueueAddress(VirtioQueueAvailableLow, queue_physical_address + AvailableOffset);
    WriteQueueAddress(VirtioQueueUsedLow, queue_physical_address + UsedOffset);
    registers[VirtioQueueReady / 4] = 1;
    return true;
}

} // namespace

bool Initialize(const void* platform_data, uint64_t hhdm_offset)
{
    uintptr_t base;
    if (!FindDevice(platform_data, &base))
    {
        return false;
    }
    device_base = base;
    device_hhdm_offset = hhdm_offset;

    volatile uint32_t* registers = Registers();
    registers[VirtioDeviceStatus / 4] = 0;
    registers[VirtioDeviceStatus / 4] = StatusAcknowledge;
    registers[VirtioDeviceStatus / 4] = StatusAcknowledge | StatusDriver;
    registers[VirtioDeviceFeaturesSelect / 4] = 0;
    (void)registers[VirtioDeviceFeatures / 4];
    registers[VirtioDeviceFeaturesSelect / 4] = 1;
    if ((registers[VirtioDeviceFeatures / 4] & FeatureVersionOne) == 0)
    {
        registers[VirtioDeviceStatus / 4] |= StatusFailed;
        return false;
    }
    registers[VirtioDriverFeaturesSelect / 4] = 0;
    registers[VirtioDriverFeatures / 4] = 0;
    registers[VirtioDriverFeaturesSelect / 4] = 1;
    registers[VirtioDriverFeatures / 4] = FeatureVersionOne;
    registers[VirtioDeviceStatus / 4] |= StatusFeaturesOk;
    if ((registers[VirtioDeviceStatus / 4] & StatusFeaturesOk) == 0 || !InitializeQueue())
    {
        registers[VirtioDeviceStatus / 4] |= StatusFailed;
        return false;
    }

    const uint64_t low_capacity = registers[VirtioConfig / 4];
    const uint64_t high_capacity = registers[(VirtioConfig + 4) / 4];
    capacity_sectors = low_capacity | (high_capacity << 32);
    registers[VirtioDeviceStatus / 4] |= StatusDriverOk;
    asm volatile("dsb sy" ::: "memory");
    initialized = capacity_sectors > 0;
    return initialized;
}

uint64_t SectorCount()
{
    return initialized ? capacity_sectors : 0;
}

bool ReadSector(uint64_t sector, void* buffer)
{
    if (!initialized || buffer == nullptr || sector >= capacity_sectors)
    {
        return false;
    }

    auto* request = reinterpret_cast<volatile BlockRequest*>(queue_virtual_address + RequestOffset);
    auto* data = queue_virtual_address + DataOffset;
    auto* status = queue_virtual_address + StatusOffset;
    auto* available_index =
        reinterpret_cast<volatile uint16_t*>(queue_virtual_address + AvailableOffset + 2);
    auto* available_ring =
        reinterpret_cast<volatile uint16_t*>(queue_virtual_address + AvailableOffset + 4);
    auto* used_index = reinterpret_cast<volatile uint16_t*>(queue_virtual_address + UsedOffset + 2);
    auto* used_elements =
        reinterpret_cast<volatile UsedElement*>(queue_virtual_address + UsedOffset + 4);

    request->type = RequestRead;
    request->reserved = 0;
    request->sector = sector;
    *status = RequestStatusPending;
    const uint16_t expected_used_index = static_cast<uint16_t>(*used_index + 1);
    available_ring[available_index_value % QueueSize] = 0;
    ++available_index_value;
    asm volatile("dmb oshst" ::: "memory");
    *available_index = available_index_value;
    Registers()[VirtioQueueNotify / 4] = 0;

    uint64_t attempts = 0;
    while (*used_index != expected_used_index && attempts < 10000000)
    {
        ++attempts;
    }
    if (*used_index != expected_used_index || used_elements[0].id != 0 || *status != 0)
    {
        return false;
    }

    asm volatile("dmb oshld" ::: "memory");
    auto* destination = static_cast<uint8_t*>(buffer);
    for (uint32_t index = 0; index < SectorSize; ++index)
    {
        destination[index] = data[index];
    }
    return true;
}

} // namespace FeatherOS::BlockDevice