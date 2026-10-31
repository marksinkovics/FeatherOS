#include "kernel/arch/virtio_net.h"

#include "interrupts.h"
#include "virtio_mmio.h"

namespace FeatherOS::Arch::VirtioNet
{
namespace
{

constexpr uint32_t DeviceFeatures = 0x010;
constexpr uint32_t DeviceFeaturesSelect = 0x014;
constexpr uint32_t DriverFeatures = 0x020;
constexpr uint32_t DriverFeaturesSelect = 0x024;
constexpr uint32_t QueueSelect = 0x030;
constexpr uint32_t QueueSizeMaximum = 0x034;
constexpr uint32_t QueueSizeRegister = 0x038;
constexpr uint32_t QueueReady = 0x044;
constexpr uint32_t QueueNotify = 0x050;
constexpr uint32_t InterruptStatus = 0x060;
constexpr uint32_t InterruptAcknowledge = 0x064;
constexpr uint32_t DeviceStatus = 0x070;
constexpr uint32_t QueueDescriptorLow = 0x080;
constexpr uint32_t QueueAvailableLow = 0x090;
constexpr uint32_t QueueUsedLow = 0x0A0;
constexpr uint32_t DeviceConfig = 0x100;
constexpr uint32_t StatusAcknowledge = 1;
constexpr uint32_t StatusDriver = 2;
constexpr uint32_t StatusDriverOk = 4;
constexpr uint32_t StatusFeaturesOk = 8;
constexpr uint32_t StatusFailed = 128;
constexpr uint32_t FeatureMac = 1U << 5;
// Bit 32, in the second feature word.
constexpr uint32_t FeatureVersionOne = 1U;

AArch64::VirtioMmio::Device device = {};

volatile uint32_t* Registers()
{
    return reinterpret_cast<volatile uint32_t*>(device.base);
}

void WriteQueueAddress(uint32_t offset, uint64_t address)
{
    Registers()[offset / 4] = static_cast<uint32_t>(address);
    Registers()[(offset + 4) / 4] = static_cast<uint32_t>(address >> 32);
}

} // namespace

bool Find(const void* platform_data)
{
    return AArch64::VirtioMmio::Find(platform_data, AArch64::VirtioMmio::NetworkDeviceId,
                                     &device) &&
           device.interrupt_id != 0;
}

bool Negotiate()
{
    Reset();
    volatile uint32_t* registers = Registers();
    registers[DeviceStatus / 4] = StatusAcknowledge;
    registers[DeviceStatus / 4] = StatusAcknowledge | StatusDriver;
    registers[DeviceFeaturesSelect / 4] = 0;
    const uint32_t low_features = registers[DeviceFeatures / 4];
    registers[DeviceFeaturesSelect / 4] = 1;
    const uint32_t high_features = registers[DeviceFeatures / 4];
    if ((low_features & FeatureMac) == 0 || (high_features & FeatureVersionOne) == 0)
    {
        registers[DeviceStatus / 4] |= StatusFailed;
        return false;
    }
    registers[DriverFeaturesSelect / 4] = 0;
    registers[DriverFeatures / 4] = FeatureMac;
    registers[DriverFeaturesSelect / 4] = 1;
    registers[DriverFeatures / 4] = FeatureVersionOne;
    registers[DeviceStatus / 4] |= StatusFeaturesOk;
    if ((registers[DeviceStatus / 4] & StatusFeaturesOk) == 0)
    {
        registers[DeviceStatus / 4] |= StatusFailed;
        return false;
    }
    return true;
}

void Reset()
{
    volatile uint32_t* registers = Registers();
    registers[DeviceStatus / 4] = 0;
    // Virtio 1.0 4.2.2.1: the reset is done once the status reads back as 0.
    while (registers[DeviceStatus / 4] != 0)
    {
        asm volatile("yield" ::: "memory");
    }
}

uint32_t HeaderSize()
{
    return 12;
}

void ReadMacAddress(uint8_t* address)
{
    const auto* config = reinterpret_cast<volatile uint8_t*>(device.base + DeviceConfig);
    for (uint32_t index = 0; index < 6; ++index)
    {
        address[index] = config[index];
    }
}

uint16_t QueueSize(uint16_t queue, uint16_t preferred)
{
    Registers()[QueueSelect / 4] = queue;
    const uint32_t maximum = Registers()[QueueSizeMaximum / 4];
    return maximum < preferred ? static_cast<uint16_t>(maximum) : preferred;
}

bool SetQueue(uint16_t queue, const Virtio::Virtqueue& virtqueue)
{
    volatile uint32_t* registers = Registers();
    registers[QueueSelect / 4] = queue;
    if (registers[QueueSizeMaximum / 4] < virtqueue.Size())
    {
        return false;
    }
    registers[QueueSizeRegister / 4] = virtqueue.Size();
    WriteQueueAddress(QueueDescriptorLow, virtqueue.DescriptorAddress());
    WriteQueueAddress(QueueAvailableLow, virtqueue.AvailableAddress());
    WriteQueueAddress(QueueUsedLow, virtqueue.UsedAddress());
    registers[QueueReady / 4] = 1;
    return true;
}

bool Start(void (*interrupt_handler)())
{
    AArch64::RegisterInterruptHandler(device.interrupt_id, interrupt_handler);
    Registers()[DeviceStatus / 4] |= StatusDriverOk;
    asm volatile("dsb sy" ::: "memory");
    return true;
}

void Notify(uint16_t queue)
{
    asm volatile("dsb sy" ::: "memory");
    Registers()[QueueNotify / 4] = queue;
}

bool AcknowledgeInterrupt()
{
    volatile uint32_t* registers = Registers();
    const uint32_t status = registers[InterruptStatus / 4];
    registers[InterruptAcknowledge / 4] = status;
    return (status & 1U) != 0;
}

} // namespace FeatherOS::Arch::VirtioNet
