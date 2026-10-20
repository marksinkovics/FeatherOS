#include "kernel/storage/block_device.h"

#include "kernel/memory/page_allocator.h"
#include "kernel/sync/spinlock.h"

#include <stdint.h>

namespace FeatherOS::BlockDevice
{
namespace
{

constexpr uint16_t PciConfigAddress = 0x0CF8;
constexpr uint16_t PciConfigData = 0x0CFC;
constexpr uint16_t VirtioVendorId = 0x1AF4;
constexpr uint16_t VirtioBlockLegacyDeviceId = 0x1001;
constexpr uint16_t VirtioHostFeatures = 0x00;
constexpr uint16_t VirtioGuestFeatures = 0x04;
constexpr uint16_t VirtioQueuePfn = 0x08;
constexpr uint16_t VirtioQueueSize = 0x0C;
constexpr uint16_t VirtioQueueSelect = 0x0E;
constexpr uint16_t VirtioQueueNotify = 0x10;
constexpr uint16_t VirtioDeviceStatus = 0x12;
constexpr uint16_t VirtioDeviceConfig = 0x14;
constexpr uint16_t PciCommandRegister = 0x04;
constexpr uint16_t PciBar0 = 0x10;
constexpr uint16_t PciCommandIoSpace = 1U;
constexpr uint16_t PciCommandBusMaster = 1U << 2;
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
constexpr uint32_t RequestRead = 0;
constexpr uint32_t RequestWrite = 1;
constexpr uint32_t FeatureReadOnly = 1U << 5;
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
bool initialized = false;
// One request buffer serves every caller, so requests from different CPUs take turns.
Sync::SpinLock device_lock;

void WritePort8(uint16_t port, uint8_t value)
{
    asm volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

void WritePort16(uint16_t port, uint16_t value)
{
    asm volatile("outw %0, %1" : : "a"(value), "Nd"(port));
}

void WritePort32(uint16_t port, uint32_t value)
{
    asm volatile("outl %0, %1" : : "a"(value), "Nd"(port));
}

uint8_t ReadPort8(uint16_t port)
{
    uint8_t value;
    asm volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

uint16_t ReadPort16(uint16_t port)
{
    uint16_t value;
    asm volatile("inw %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

uint32_t ReadPort32(uint16_t port)
{
    uint32_t value;
    asm volatile("inl %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

uint32_t ReadPciConfig(uint8_t bus, uint8_t slot, uint8_t function, uint8_t offset)
{
    const uint32_t address = 0x80000000U | (static_cast<uint32_t>(bus) << 16) |
                             (static_cast<uint32_t>(slot) << 11) |
                             (static_cast<uint32_t>(function) << 8) | (offset & 0xFCU);
    WritePort32(PciConfigAddress, address);
    return ReadPort32(PciConfigData);
}

void WritePciConfig(uint8_t bus, uint8_t slot, uint8_t function, uint8_t offset, uint32_t value)
{
    const uint32_t address = 0x80000000U | (static_cast<uint32_t>(bus) << 16) |
                             (static_cast<uint32_t>(slot) << 11) |
                             (static_cast<uint32_t>(function) << 8) | (offset & 0xFCU);
    WritePort32(PciConfigAddress, address);
    WritePort32(PciConfigData, value);
}

bool FindVirtioBlockDevice()
{
    for (uint32_t bus = 0; bus < 256; ++bus)
    {
        for (uint32_t slot = 0; slot < 32; ++slot)
        {
            for (uint32_t function = 0; function < 8; ++function)
            {
                const uint32_t identity =
                    ReadPciConfig(static_cast<uint8_t>(bus), static_cast<uint8_t>(slot),
                                  static_cast<uint8_t>(function), 0);
                const uint16_t vendor = static_cast<uint16_t>(identity);
                const uint16_t device = static_cast<uint16_t>(identity >> 16);
                if (vendor == 0xFFFF)
                {
                    break;
                }
                if (vendor != VirtioVendorId || device != VirtioBlockLegacyDeviceId)
                {
                    continue;
                }

                const uint32_t bar0 =
                    ReadPciConfig(static_cast<uint8_t>(bus), static_cast<uint8_t>(slot),
                                  static_cast<uint8_t>(function), PciBar0);
                if ((bar0 & 1U) == 0 || (bar0 & 0xFFFCU) == 0)
                {
                    return false;
                }

                uint32_t command =
                    ReadPciConfig(static_cast<uint8_t>(bus), static_cast<uint8_t>(slot),
                                  static_cast<uint8_t>(function), PciCommandRegister);
                command = (command & 0xFFFF0000U) |
                          ((command & 0xFFFFU) | PciCommandIoSpace | PciCommandBusMaster);
                WritePciConfig(static_cast<uint8_t>(bus), static_cast<uint8_t>(slot),
                               static_cast<uint8_t>(function), PciCommandRegister, command);
                device_io_base = static_cast<uint16_t>(bar0 & 0xFFFCU);
                return true;
            }
        }
    }
    return false;
}

bool AllocateQueue(uint64_t hhdm_offset)
{
    for (uint32_t attempt = 0; attempt < 16; ++attempt)
    {
        uint64_t frames[QueuePageCount] = {};
        uint32_t allocated_count = 0;
        for (; allocated_count < QueuePageCount; ++allocated_count)
        {
            if (!Memory::AllocatePageFrame(&frames[allocated_count]))
            {
                break;
            }
        }
        if (allocated_count != QueuePageCount)
        {
            for (uint32_t index = 0; index < allocated_count; ++index)
            {
                Memory::FreePageFrame(frames[index]);
            }
            return false;
        }

        bool contiguous = true;
        for (uint32_t index = 1; index < QueuePageCount; ++index)
        {
            if (frames[index - 1] > UINT64_MAX - QueuePageSize ||
                frames[index] != frames[index - 1] + QueuePageSize)
            {
                contiguous = false;
                break;
            }
        }
        if (contiguous && frames[0] <= UINT64_MAX - hhdm_offset &&
            hhdm_offset + frames[0] <= UINTPTR_MAX)
        {
            queue_physical_address = frames[0];
            queue_virtual_address = reinterpret_cast<volatile uint8_t*>(
                static_cast<uintptr_t>(hhdm_offset + frames[0]));
            for (uint32_t offset = 0; offset < QueuePageCount * QueuePageSize; ++offset)
            {
                queue_virtual_address[offset] = 0;
            }
            return true;
        }
        for (uint32_t index = 0; index < QueuePageCount; ++index)
        {
            Memory::FreePageFrame(frames[index]);
        }
    }
    return false;
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
    auto* available_index =
        reinterpret_cast<volatile uint16_t*>(queue_virtual_address + AvailableOffset + 2);
    auto* available_ring =
        reinterpret_cast<volatile uint16_t*>(queue_virtual_address + AvailableOffset + 4);
    auto* used_index = reinterpret_cast<volatile uint16_t*>(queue_virtual_address + UsedOffset + 2);
    auto* used_elements =
        reinterpret_cast<volatile UsedElement*>(queue_virtual_address + UsedOffset + 4);
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
    queue_virtual_address[StatusOffset] = RequestStatusPending;
    const uint16_t used_slot = *used_index % QueueSize;
    const uint16_t expected_used_index = static_cast<uint16_t>(*used_index + 1);
    available_ring[available_index_value % QueueSize] = 0;
    ++available_index_value;
    asm volatile("mfence" ::: "memory");
    *available_index = available_index_value;
    WritePort16(device_io_base + VirtioQueueNotify, QueueIndex);

    uint64_t attempts = 0;
    while (*used_index != expected_used_index && attempts < 10000000)
    {
        ++attempts;
    }
    if (*used_index != expected_used_index || used_elements[used_slot].id != 0 ||
        queue_virtual_address[StatusOffset] != 0)
    {
        return false;
    }

    asm volatile("lfence" ::: "memory");
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
    if (initialized || !FindVirtioBlockDevice())
    {
        return false;
    }

    WritePort8(device_io_base + VirtioDeviceStatus, 0);
    WritePort8(device_io_base + VirtioDeviceStatus, StatusAcknowledge);
    WritePort8(device_io_base + VirtioDeviceStatus, StatusAcknowledge | StatusDriver);
    read_only = (ReadPort32(device_io_base + VirtioHostFeatures) & FeatureReadOnly) != 0;
    WritePort32(device_io_base + VirtioGuestFeatures, 0);
    WritePort16(device_io_base + VirtioQueueSelect, QueueIndex);
    if (ReadPort16(device_io_base + VirtioQueueSize) < QueueSize || !AllocateQueue(hhdm_offset))
    {
        WritePort8(device_io_base + VirtioDeviceStatus, StatusAcknowledge | StatusDriver | 128);
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
    available_ring[0] = 0;
    *available_index = 0;
    asm volatile("mfence" ::: "memory");
    WritePort32(device_io_base + VirtioQueuePfn,
                static_cast<uint32_t>(queue_physical_address / QueuePageSize));

    capacity_sectors = ReadPort32(device_io_base + VirtioDeviceConfig);
    capacity_sectors |= static_cast<uint64_t>(ReadPort32(device_io_base + VirtioDeviceConfig + 4))
                        << 32;
    if (capacity_sectors == 0)
    {
        return false;
    }
    WritePort8(device_io_base + VirtioDeviceStatus,
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

} // namespace FeatherOS::BlockDevice