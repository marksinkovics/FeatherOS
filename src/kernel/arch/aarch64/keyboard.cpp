#include "keyboard.h"

#include "kernel/arch/keyboard.h"
#include "interrupts.h"
#include "kernel/input/input.h"
#include "kernel/input/keyboard.h"
#include "kernel/memory/device_memory.h"
#include "kernel/memory/page_allocator.h"

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

constexpr uint32_t VirtioMagic = 0x74726976;
constexpr uint32_t VirtioInputDeviceId = 18;
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
constexpr uint32_t VirtioInterruptStatus = 0x060;
constexpr uint32_t VirtioInterruptAcknowledge = 0x064;
constexpr uint32_t VirtioDeviceStatus = 0x070;
constexpr uint32_t VirtioQueueDescriptorLow = 0x080;
constexpr uint32_t VirtioQueueAvailableLow = 0x090;
constexpr uint32_t VirtioQueueUsedLow = 0x0A0;

constexpr uint32_t StatusAcknowledge = 1U;
constexpr uint32_t StatusDriver = 2U;
constexpr uint32_t StatusDriverOk = 4U;
constexpr uint32_t StatusFeaturesOk = 8U;
constexpr uint32_t StatusFailed = 128U;
constexpr uint32_t FeatureVersionOne = 1U;
constexpr uint32_t QueueSize = 8;
constexpr uint32_t DescriptorOffset = 0;
constexpr uint32_t AvailableOffset = sizeof(uint64_t) * 2 * QueueSize;
constexpr uint32_t UsedOffset = (AvailableOffset + 4 + 2 * QueueSize + 3) & ~3U;
constexpr uint32_t EventOffset = (UsedOffset + 4 + 8 * QueueSize + 7) & ~7U;

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

struct DeviceRegisters
{
    uintptr_t base;
    uint32_t interrupt_id;
};

struct __attribute__((packed)) VirtqueueDescriptor
{
    uint64_t address;
    uint32_t length;
    uint16_t flags;
    uint16_t next;
};

struct __attribute__((packed)) VirtqueueUsedElement
{
    uint32_t id;
    uint32_t length;
};

struct VirtioInputEvent
{
    uint16_t type;
    uint16_t code;
    uint32_t value;
};

uintptr_t device_base = 0;
uint32_t device_interrupt_id = 0;
uint64_t queue_physical_address = 0;
uint64_t device_hhdm_offset = 0;
volatile uint8_t* queue_virtual_address = nullptr;
uint16_t last_used_index = 0;
Input::Keyboard keyboard;

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

bool ReadDeviceRegisters(const FdtNode& node, DeviceRegisters* registers)
{
    if (node.reg == nullptr || node.interrupts == nullptr || node.reg_address_cells == 0 ||
        node.reg_address_cells > 2 || node.reg_size_cells > 2 || node.interrupts_length < 12)
    {
        return false;
    }
    const uint32_t tuple_cells = node.reg_address_cells + node.reg_size_cells;
    if (tuple_cells == 0 || node.reg_length < tuple_cells * 4)
    {
        return false;
    }

    uint64_t address;
    if (!ReadCells(node.reg, node.reg_address_cells, &address) || address > UINTPTR_MAX)
    {
        return false;
    }

    const uint32_t interrupt_type = ReadBigEndian32(node.interrupts);
    const uint32_t interrupt_number = ReadBigEndian32(node.interrupts + 4);
    if (interrupt_type == 0)
    {
        registers->interrupt_id = 32 + interrupt_number;
    }
    else if (interrupt_type == 1)
    {
        registers->interrupt_id = 16 + interrupt_number;
    }
    else
    {
        return false;
    }
    constexpr uint64_t VirtioMmioRegisterSize = 0x200;
    registers->base = Memory::MapDevice(address, VirtioMmioRegisterSize);
    return registers->base != 0;
}

volatile uint32_t* Registers(uintptr_t base)
{
    return reinterpret_cast<volatile uint32_t*>(base);
}

volatile uint32_t* Registers()
{
    return Registers(device_base);
}

bool FindKeyboardDevice(const void* device_tree, DeviceRegisters* registers)
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
            const FdtNode& node = nodes[depth - 1];
            if (!node.disabled && HasCompatible(node, "virtio,mmio") &&
                ReadDeviceRegisters(node, registers))
            {
                auto* device = Registers(registers->base);
                if (device[0] == VirtioMagic && device[1] >= 2 && device[2] == VirtioInputDeviceId)
                {
                    return true;
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
                                  strings_size - name_offset, "interrupts"))
            {
                node.interrupts = value;
                node.interrupts_length = length;
            }
            else if (StringEquals(reinterpret_cast<const uint8_t*>(name),
                                  strings_size - name_offset, "status"))
            {
                node.disabled = StringEquals(value, length, "disabled");
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

void WriteQueueAddress(uint32_t offset, uint64_t address)
{
    Registers()[offset / 4] = static_cast<uint32_t>(address);
    Registers()[(offset + 4) / 4] = static_cast<uint32_t>(address >> 32);
}

// A key event's value is 0 for a release, 1 for a press and 2 for a held key's repeat.
void HandleKeyEvent(uint16_t code, uint32_t value)
{
    char bytes[Input::Keyboard::MaximumKeyBytes];
    const size_t length = keyboard.Press(code, value != 0, bytes);
    if (length > 0)
    {
        Input::SubmitBytes(bytes, length);
    }
}

void OnKeyboardInterrupt()
{
    volatile uint32_t* registers = Registers();
    const uint32_t interrupt_status = registers[VirtioInterruptStatus / 4];
    registers[VirtioInterruptAcknowledge / 4] = interrupt_status;
    if ((interrupt_status & 1U) == 0)
    {
        return;
    }

    asm volatile("dmb oshld" ::: "memory");
    auto* used_index = reinterpret_cast<volatile uint16_t*>(queue_virtual_address + UsedOffset + 2);
    auto* used_elements =
        reinterpret_cast<volatile VirtqueueUsedElement*>(queue_virtual_address + UsedOffset + 4);
    auto* available_index =
        reinterpret_cast<volatile uint16_t*>(queue_virtual_address + AvailableOffset + 2);
    auto* available_ring =
        reinterpret_cast<volatile uint16_t*>(queue_virtual_address + AvailableOffset + 4);
    auto* events =
        reinterpret_cast<volatile VirtioInputEvent*>(queue_virtual_address + EventOffset);

    while (last_used_index != *used_index)
    {
        const uint32_t descriptor_id = used_elements[last_used_index % QueueSize].id;
        if (descriptor_id < QueueSize)
        {
            const uint16_t event_type = events[descriptor_id].type;
            const uint16_t event_code = events[descriptor_id].code;
            const uint32_t event_value = events[descriptor_id].value;
            if (event_type == 1)
            {
                HandleKeyEvent(event_code, event_value);
            }
            available_ring[*available_index % QueueSize] = static_cast<uint16_t>(descriptor_id);
            asm volatile("dmb oshst" ::: "memory");
            ++*available_index;
        }
        ++last_used_index;
    }
    registers[VirtioQueueNotify / 4] = 0;
}

bool InitializeQueue()
{
    volatile uint32_t* registers = Registers();
    registers[VirtioQueueSelect / 4] = 0;
    if (registers[VirtioQueueSizeMaximum / 4] < QueueSize)
    {
        return false;
    }

    if (!Memory::AllocatePageFrame(&queue_physical_address) ||
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

    auto* descriptors =
        reinterpret_cast<volatile VirtqueueDescriptor*>(queue_virtual_address + DescriptorOffset);
    auto* available_index =
        reinterpret_cast<volatile uint16_t*>(queue_virtual_address + AvailableOffset + 2);
    auto* available_ring =
        reinterpret_cast<volatile uint16_t*>(queue_virtual_address + AvailableOffset + 4);
    for (uint32_t index = 0; index < QueueSize; ++index)
    {
        descriptors[index].address =
            queue_physical_address + EventOffset + index * sizeof(VirtioInputEvent);
        descriptors[index].length = sizeof(VirtioInputEvent);
        descriptors[index].flags = 2;
        descriptors[index].next = 0;
        available_ring[index] = static_cast<uint16_t>(index);
    }
    asm volatile("dmb oshst" ::: "memory");
    *available_index = QueueSize;

    registers[VirtioQueueSize / 4] = QueueSize;
    WriteQueueAddress(VirtioQueueDescriptorLow, queue_physical_address + DescriptorOffset);
    WriteQueueAddress(VirtioQueueAvailableLow, queue_physical_address + AvailableOffset);
    WriteQueueAddress(VirtioQueueUsedLow, queue_physical_address + UsedOffset);
    registers[VirtioQueueReady / 4] = 1;
    return true;
}

} // namespace

bool InitializeKeyboard(const void* device_tree, uint64_t hhdm_offset)
{
    DeviceRegisters device_registers;
    if (!FindKeyboardDevice(device_tree, &device_registers))
    {
        return false;
    }

    device_base = device_registers.base;
    device_interrupt_id = device_registers.interrupt_id;
    device_hhdm_offset = hhdm_offset;

    volatile uint32_t* registers = Registers();
    registers[VirtioDeviceStatus / 4] = 0;
    registers[VirtioDeviceStatus / 4] = StatusAcknowledge;
    registers[VirtioDeviceStatus / 4] = StatusAcknowledge | StatusDriver;

    registers[VirtioDeviceFeaturesSelect / 4] = 0;
    (void)registers[VirtioDeviceFeatures / 4];
    registers[VirtioDeviceFeaturesSelect / 4] = 1;
    const uint32_t device_features_high = registers[VirtioDeviceFeatures / 4];
    if ((device_features_high & FeatureVersionOne) == 0)
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

    RegisterInterruptHandler(device_interrupt_id, OnKeyboardInterrupt);
    registers[VirtioDeviceStatus / 4] |= StatusDriverOk;
    asm volatile("dsb sy" ::: "memory");
    registers[VirtioQueueNotify / 4] = 0;
    return true;
}

} // namespace FeatherOS::Arch::AArch64

namespace FeatherOS::Arch::Keyboard
{

bool Initialize(const void* platform_data, uint64_t hhdm_offset)
{
    return AArch64::InitializeKeyboard(platform_data, hhdm_offset);
}

} // namespace FeatherOS::Arch::Keyboard