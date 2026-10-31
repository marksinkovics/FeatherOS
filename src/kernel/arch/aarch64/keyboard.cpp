#include "keyboard.h"

#include "kernel/arch/keyboard.h"
#include "interrupts.h"
#include "virtio_mmio.h"
#include "kernel/input/input.h"
#include "kernel/input/keyboard.h"
#include "kernel/memory/page_allocator.h"

#include <stddef.h>
#include <stdint.h>

namespace FeatherOS::Arch::AArch64
{
namespace
{

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

volatile uint32_t* Registers()
{
    return reinterpret_cast<volatile uint32_t*>(device_base);
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
            *available_index = *available_index + 1;
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
    VirtioMmio::Device device;
    if (!VirtioMmio::Find(device_tree, VirtioMmio::InputDeviceId, &device) ||
        device.interrupt_id == 0)
    {
        return false;
    }

    device_base = device.base;
    device_interrupt_id = device.interrupt_id;
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