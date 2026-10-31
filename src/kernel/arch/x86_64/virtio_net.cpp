#include "kernel/arch/virtio_net.h"

#include "interrupts.h"
#include "pci.h"
#include "port_io.h"

namespace FeatherOS::Arch::VirtioNet
{
namespace
{

namespace PortIo = X86_64::PortIo;

constexpr uint16_t HostFeatures = 0x00;
constexpr uint16_t GuestFeatures = 0x04;
constexpr uint16_t QueuePfn = 0x08;
constexpr uint16_t QueueSizeRegister = 0x0C;
constexpr uint16_t QueueSelect = 0x0E;
constexpr uint16_t QueueNotify = 0x10;
constexpr uint16_t DeviceStatus = 0x12;
constexpr uint16_t InterruptStatus = 0x13;
// Without MSI-X the device-specific configuration follows the common registers.
constexpr uint16_t DeviceConfig = 0x14;
constexpr uint8_t StatusAcknowledge = 1;
constexpr uint8_t StatusDriver = 2;
constexpr uint8_t StatusDriverOk = 4;
constexpr uint8_t StatusFailed = 128;
constexpr uint32_t FeatureMac = 1U << 5;
constexpr uint32_t FeatureAnyLayout = 1U << 27;
constexpr uint32_t LegacyQueueAlignment = 4096;
constexpr uint8_t NoInterruptLine = 0xFF;

uint16_t io_base = 0;
uint8_t interrupt_line = NoInterruptLine;

} // namespace

bool Find(const void*)
{
    X86_64::Pci::Device device;
    if (!X86_64::Pci::FindIoDevice(X86_64::Pci::VirtioVendorId,
                                   X86_64::Pci::VirtioNetworkLegacyDeviceId, &device) ||
        device.interrupt_line >= 16)
    {
        return false;
    }
    io_base = device.io_base;
    interrupt_line = device.interrupt_line;
    return true;
}

bool Negotiate()
{
    Reset();
    PortIo::Write8(io_base + DeviceStatus, StatusAcknowledge);
    PortIo::Write8(io_base + DeviceStatus, StatusAcknowledge | StatusDriver);
    const uint32_t features = PortIo::Read32(io_base + HostFeatures);
    if ((features & FeatureMac) == 0)
    {
        PortIo::Write8(io_base + DeviceStatus, StatusAcknowledge | StatusDriver | StatusFailed);
        return false;
    }
    // Any layout lets the header and the frame share one descriptor.
    PortIo::Write32(io_base + GuestFeatures, features & (FeatureMac | FeatureAnyLayout));
    return true;
}

void Reset()
{
    // A legacy device resets at once when its status becomes 0.
    PortIo::Write8(io_base + DeviceStatus, 0);
}

uint32_t HeaderSize()
{
    return 10;
}

void ReadMacAddress(uint8_t* address)
{
    for (uint16_t index = 0; index < 6; ++index)
    {
        address[index] = PortIo::Read8(io_base + DeviceConfig + index);
    }
}

uint16_t QueueSize(uint16_t queue, uint16_t)
{
    PortIo::Write16(io_base + QueueSelect, queue);
    return PortIo::Read16(io_base + QueueSizeRegister);
}

bool SetQueue(uint16_t queue, const Virtio::Virtqueue& virtqueue)
{
    PortIo::Write16(io_base + QueueSelect, queue);
    if (PortIo::Read16(io_base + QueueSizeRegister) != virtqueue.Size() ||
        virtqueue.DescriptorAddress() % LegacyQueueAlignment != 0 ||
        virtqueue.DescriptorAddress() / LegacyQueueAlignment > UINT32_MAX)
    {
        return false;
    }
    PortIo::Write32(io_base + QueuePfn,
                    static_cast<uint32_t>(virtqueue.DescriptorAddress() / LegacyQueueAlignment));
    return true;
}

bool Start(void (*interrupt_handler)())
{
    X86_64::RegisterInterruptHandler(interrupt_line, interrupt_handler);
    PortIo::Write8(io_base + DeviceStatus, StatusAcknowledge | StatusDriver | StatusDriverOk);
    X86_64::UnmaskIrq(interrupt_line);
    return true;
}

void Notify(uint16_t queue)
{
    PortIo::Write16(io_base + QueueNotify, queue);
}

bool AcknowledgeInterrupt()
{
    // Reading the status clears it and lowers the level-triggered line.
    return (PortIo::Read8(io_base + InterruptStatus) & 1U) != 0;
}

} // namespace FeatherOS::Arch::VirtioNet
