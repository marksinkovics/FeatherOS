#include "kernel/storage/block_device.h"

#include "virtio_mmio.h"

#include "kernel/memory/page_allocator.h"
#include "kernel/sync/spinlock.h"

#include <stddef.h>
#include <stdint.h>

namespace FeatherOS::BlockDevice
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
constexpr uint32_t RequestWrite = 1;
constexpr uint32_t RequestFlush = 4;
constexpr uint32_t FeatureReadOnly = 1U << 5;
constexpr uint32_t FeatureFlush = 1U << 9;
constexpr uint8_t RequestStatusPending = 0xFF;

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
bool read_only = false;
bool flush_supported = false;
bool initialized = false;
// One request buffer serves every caller, so requests from different CPUs take turns.
constinit Sync::SpinLock device_lock;

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

// Hands the prepared request to the device and polls until it completes.
bool SubmitLocked()
{
    auto* status = queue_virtual_address + StatusOffset;
    auto* available_index =
        reinterpret_cast<volatile uint16_t*>(queue_virtual_address + AvailableOffset + 2);
    auto* available_ring =
        reinterpret_cast<volatile uint16_t*>(queue_virtual_address + AvailableOffset + 4);
    auto* used_index = reinterpret_cast<volatile uint16_t*>(queue_virtual_address + UsedOffset + 2);
    auto* used_elements =
        reinterpret_cast<volatile UsedElement*>(queue_virtual_address + UsedOffset + 4);
    *status = RequestStatusPending;
    const uint16_t used_slot = *used_index % QueueSize;
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
    asm volatile("dmb oshld" ::: "memory");
    return *used_index == expected_used_index && used_elements[used_slot].id == 0 && *status == 0;
}

// Writes source to the sector when it is set, otherwise reads the sector into destination.
bool Transfer(uint64_t sector, const uint8_t* source, uint8_t* destination)
{
    const bool write = source != nullptr;
    if (!initialized || (write ? read_only : destination == nullptr) || sector >= capacity_sectors)
    {
        return false;
    }

    Sync::SpinLockGuard guard(device_lock);
    auto* descriptors = reinterpret_cast<volatile Descriptor*>(queue_virtual_address);
    auto* request = reinterpret_cast<volatile BlockRequest*>(queue_virtual_address + RequestOffset);
    auto* data = queue_virtual_address + DataOffset;

    request->type = write ? RequestWrite : RequestRead;
    request->reserved = 0;
    request->sector = sector;
    // The device writes the data buffer for reads and reads it for writes.
    descriptors[1].flags = write ? DescriptorNext : DescriptorNext | DescriptorDeviceWrite;
    if (write)
    {
        for (uint32_t index = 0; index < SectorSize; ++index)
        {
            data[index] = source[index];
        }
    }
    if (!SubmitLocked())
    {
        return false;
    }
    if (!write)
    {
        for (uint32_t index = 0; index < SectorSize; ++index)
        {
            destination[index] = data[index];
        }
    }
    return true;
}

} // namespace

bool Initialize(const void* platform_data, uint64_t hhdm_offset)
{
    Arch::AArch64::VirtioMmio::Device device;
    if (!Arch::AArch64::VirtioMmio::Find(platform_data, Arch::AArch64::VirtioMmio::BlockDeviceId,
                                         &device))
    {
        return false;
    }
    device_base = device.base;
    device_hhdm_offset = hhdm_offset;

    volatile uint32_t* registers = Registers();
    registers[VirtioDeviceStatus / 4] = 0;
    registers[VirtioDeviceStatus / 4] = StatusAcknowledge;
    registers[VirtioDeviceStatus / 4] = StatusAcknowledge | StatusDriver;
    registers[VirtioDeviceFeaturesSelect / 4] = 0;
    const uint32_t device_features = registers[VirtioDeviceFeatures / 4];
    read_only = (device_features & FeatureReadOnly) != 0;
    flush_supported = (device_features & FeatureFlush) != 0;
    registers[VirtioDeviceFeaturesSelect / 4] = 1;
    if ((registers[VirtioDeviceFeatures / 4] & FeatureVersionOne) == 0)
    {
        registers[VirtioDeviceStatus / 4] |= StatusFailed;
        return false;
    }
    registers[VirtioDriverFeaturesSelect / 4] = 0;
    registers[VirtioDriverFeatures / 4] = device_features & FeatureFlush;
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
    return Transfer(sector, nullptr, static_cast<uint8_t*>(buffer));
}

bool WriteSector(uint64_t sector, const void* buffer)
{
    return Transfer(sector, static_cast<const uint8_t*>(buffer), nullptr);
}

bool Flush()
{
    // Without the flush feature the device completes writes before reporting them done.
    if (!initialized || !flush_supported)
    {
        return initialized;
    }
    Sync::SpinLockGuard guard(device_lock);
    auto* descriptors = reinterpret_cast<volatile Descriptor*>(queue_virtual_address);
    auto* request = reinterpret_cast<volatile BlockRequest*>(queue_virtual_address + RequestOffset);
    request->type = RequestFlush;
    request->reserved = 0;
    request->sector = 0;
    // A flush has no data, so the header leads straight to the status byte.
    descriptors[0].next = 2;
    const bool flushed = SubmitLocked();
    descriptors[0].next = 1;
    return flushed;
}

} // namespace FeatherOS::BlockDevice