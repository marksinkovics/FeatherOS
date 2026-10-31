#include "kernel/storage/block_device.h"

#include "pci.h"
#include "port_io.h"

#include "kernel/memory/page_allocator.h"
#include "kernel/sync/spinlock.h"

#include <stdint.h>

namespace FeatherOS::BlockDevice
{
namespace
{

namespace PortIo = Arch::X86_64::PortIo;

constexpr uint16_t VirtioHostFeatures = 0x00;
constexpr uint16_t VirtioGuestFeatures = 0x04;
constexpr uint16_t VirtioQueuePfn = 0x08;
constexpr uint16_t VirtioQueueSize = 0x0C;
constexpr uint16_t VirtioQueueSelect = 0x0E;
constexpr uint16_t VirtioQueueNotify = 0x10;
constexpr uint16_t VirtioDeviceStatus = 0x12;
constexpr uint16_t VirtioDeviceConfig = 0x14;
constexpr uint16_t StatusAcknowledge = 1U;
constexpr uint16_t StatusDriver = 2U;
constexpr uint16_t StatusDriverOk = 4U;
constexpr uint16_t QueueIndex = 0;
constexpr uint16_t QueueSize = 256;
constexpr uint32_t QueuePageSize = 4096;
constexpr uint32_t QueuePageCount = 3;
constexpr uint32_t DescriptorOffset = 0;
constexpr uint32_t AvailableOffset = sizeof(uint64_t) * 2 * QueueSize;
constexpr uint32_t UsedOffset =
    (AvailableOffset + 4 + 2 * QueueSize + QueuePageSize - 1) & ~(QueuePageSize - 1);
constexpr uint32_t RequestOffset = UsedOffset + 4 + 8 * QueueSize;
constexpr uint32_t DataOffset = RequestOffset + 16;
constexpr uint32_t StatusOffset = DataOffset + SectorSize;
constexpr uint16_t DescriptorNext = 1;
constexpr uint16_t DescriptorDeviceWrite = 2;
constexpr uint16_t AvailableNoInterrupt = 1;
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

struct Request
{
    uint32_t type;
    uint32_t reserved;
    uint64_t sector;
};

uint16_t device_io_base = 0;
uint64_t queue_physical_address = 0;
volatile uint8_t* queue_virtual_address = nullptr;
uint64_t capacity_sectors = 0;
uint16_t available_index_value = 0;
bool read_only = false;
bool flush_supported = false;
bool initialized = false;
// One request buffer serves every caller, so requests from different CPUs take turns.
constinit Sync::SpinLock device_lock;

bool AllocateQueue(uint64_t hhdm_offset)
{
    uint64_t physical;
    if (!Memory::AllocateContiguousPageFrames(QueuePageCount, &physical))
    {
        return false;
    }
    if (physical > UINT64_MAX - hhdm_offset || hhdm_offset + physical > UINTPTR_MAX)
    {
        for (uint32_t index = 0; index < QueuePageCount; ++index)
        {
            Memory::FreePageFrame(physical + index * QueuePageSize);
        }
        return false;
    }
    queue_physical_address = physical;
    queue_virtual_address =
        reinterpret_cast<volatile uint8_t*>(static_cast<uintptr_t>(hhdm_offset + physical));
    for (uint32_t offset = 0; offset < QueuePageCount * QueuePageSize; ++offset)
    {
        queue_virtual_address[offset] = 0;
    }
    return true;
}

void ConfigureDescriptor(uint16_t index, uint64_t address, uint32_t length, uint16_t flags,
                         uint16_t next)
{
    auto* descriptors = reinterpret_cast<volatile Descriptor*>(queue_virtual_address);
    descriptors[index].address = address;
    descriptors[index].length = length;
    descriptors[index].flags = flags;
    descriptors[index].next = next;
}

// Hands the prepared request to the device and polls until it completes.
bool SubmitLocked()
{
    auto* available_index =
        reinterpret_cast<volatile uint16_t*>(queue_virtual_address + AvailableOffset + 2);
    auto* available_ring =
        reinterpret_cast<volatile uint16_t*>(queue_virtual_address + AvailableOffset + 4);
    auto* used_index = reinterpret_cast<volatile uint16_t*>(queue_virtual_address + UsedOffset + 2);
    auto* used_elements =
        reinterpret_cast<volatile UsedElement*>(queue_virtual_address + UsedOffset + 4);
    queue_virtual_address[StatusOffset] = RequestStatusPending;
    const uint16_t used_slot = *used_index % QueueSize;
    const uint16_t expected_used_index = static_cast<uint16_t>(*used_index + 1);
    available_ring[available_index_value % QueueSize] = 0;
    ++available_index_value;
    asm volatile("mfence" ::: "memory");
    *available_index = available_index_value;
    PortIo::Write16(device_io_base + VirtioQueueNotify, QueueIndex);

    uint64_t attempts = 0;
    while (*used_index != expected_used_index && attempts < 10000000)
    {
        ++attempts;
    }
    asm volatile("lfence" ::: "memory");
    return *used_index == expected_used_index && used_elements[used_slot].id == 0 &&
           queue_virtual_address[StatusOffset] == 0;
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
    auto* request = reinterpret_cast<volatile Request*>(queue_virtual_address + RequestOffset);
    volatile uint8_t* data = queue_virtual_address + DataOffset;

    request->type = write ? RequestWrite : RequestRead;
    request->reserved = 0;
    request->sector = sector;
    // The device writes the data buffer for reads and reads it for writes.
    ConfigureDescriptor(1, queue_physical_address + DataOffset, SectorSize,
                        write ? DescriptorNext : DescriptorNext | DescriptorDeviceWrite, 2);
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

bool Initialize(const void*, uint64_t hhdm_offset)
{
    Arch::X86_64::Pci::Device device;
    if (initialized ||
        !Arch::X86_64::Pci::FindIoDevice(Arch::X86_64::Pci::VirtioVendorId,
                                         Arch::X86_64::Pci::VirtioBlockLegacyDeviceId, &device))
    {
        return false;
    }
    device_io_base = device.io_base;

    PortIo::Write8(device_io_base + VirtioDeviceStatus, 0);
    PortIo::Write8(device_io_base + VirtioDeviceStatus, StatusAcknowledge);
    PortIo::Write8(device_io_base + VirtioDeviceStatus, StatusAcknowledge | StatusDriver);
    const uint32_t host_features = PortIo::Read32(device_io_base + VirtioHostFeatures);
    read_only = (host_features & FeatureReadOnly) != 0;
    flush_supported = (host_features & FeatureFlush) != 0;
    PortIo::Write32(device_io_base + VirtioGuestFeatures, host_features & FeatureFlush);
    PortIo::Write16(device_io_base + VirtioQueueSelect, QueueIndex);
    if (PortIo::Read16(device_io_base + VirtioQueueSize) < QueueSize || !AllocateQueue(hhdm_offset))
    {
        PortIo::Write8(device_io_base + VirtioDeviceStatus, StatusAcknowledge | StatusDriver | 128);
        return false;
    }

    ConfigureDescriptor(0, queue_physical_address + RequestOffset, sizeof(Request), DescriptorNext,
                        1);
    ConfigureDescriptor(1, queue_physical_address + DataOffset, SectorSize,
                        DescriptorNext | DescriptorDeviceWrite, 2);
    ConfigureDescriptor(2, queue_physical_address + StatusOffset, 1, DescriptorDeviceWrite, 0);
    auto* request = reinterpret_cast<volatile Request*>(queue_virtual_address + RequestOffset);
    request->type = RequestRead;
    request->reserved = 0;
    request->sector = 0;
    queue_virtual_address[StatusOffset] = RequestStatusPending;
    auto* available_index =
        reinterpret_cast<volatile uint16_t*>(queue_virtual_address + AvailableOffset + 2);
    auto* available_ring =
        reinterpret_cast<volatile uint16_t*>(queue_virtual_address + AvailableOffset + 4);
    // Requests are polled. Without this the device would keep raising its PCI interrupt, a line
    // that virtio-net may share.
    *reinterpret_cast<volatile uint16_t*>(queue_virtual_address + AvailableOffset) =
        AvailableNoInterrupt;
    available_ring[0] = 0;
    *available_index = 0;
    asm volatile("mfence" ::: "memory");
    PortIo::Write32(device_io_base + VirtioQueuePfn,
                    static_cast<uint32_t>(queue_physical_address / QueuePageSize));

    capacity_sectors = PortIo::Read32(device_io_base + VirtioDeviceConfig);
    capacity_sectors |=
        static_cast<uint64_t>(PortIo::Read32(device_io_base + VirtioDeviceConfig + 4)) << 32;
    if (capacity_sectors == 0)
    {
        return false;
    }
    PortIo::Write8(device_io_base + VirtioDeviceStatus,
                   StatusAcknowledge | StatusDriver | StatusDriverOk);
    initialized = true;
    return true;
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
    auto* request = reinterpret_cast<volatile Request*>(queue_virtual_address + RequestOffset);
    request->type = RequestFlush;
    request->reserved = 0;
    request->sector = 0;
    // A flush has no data, so the header leads straight to the status byte.
    ConfigureDescriptor(0, queue_physical_address + RequestOffset, sizeof(Request), DescriptorNext,
                        2);
    const bool flushed = SubmitLocked();
    ConfigureDescriptor(0, queue_physical_address + RequestOffset, sizeof(Request), DescriptorNext,
                        1);
    return flushed;
}

} // namespace FeatherOS::BlockDevice