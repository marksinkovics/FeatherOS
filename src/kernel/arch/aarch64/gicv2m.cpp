#include "interrupts.h"
#include "pci_ecam.h"

#include "kernel/arch/msi.h"
#include "kernel/memory/device_memory.h"
#include "kernel/sync/spinlock.h"

namespace FeatherOS::Arch::AArch64::Gicv2m
{
namespace
{

constexpr uint64_t FrameSize = 0x1000;
constexpr uint32_t TypeRegister = 0x008;
constexpr uint32_t SetSpiRegister = 0x040;
constexpr uint32_t MaximumMessages = 16;

constinit Sync::SpinLock lock;
uintptr_t frame_registers GUARDED_BY(lock) = 0;
uint32_t first_spi GUARDED_BY(lock) = 0;
uint32_t spi_count GUARDED_BY(lock) = 0;
uint32_t used GUARDED_BY(lock) = 0;

bool Reserve(const PciDeviceTree::HostBridge& bridge, uint32_t* interrupt_id) REQUIRES(lock)
{
    if (frame_registers == 0)
    {
        const uintptr_t registers = Memory::MapDevice(bridge.msi_frame_base, FrameSize);
        if (registers == 0)
        {
            return false;
        }
        const uint32_t type = *reinterpret_cast<volatile uint32_t*>(registers + TypeRegister);
        first_spi = (type >> 16) & 0x3FF;
        spi_count = type & 0x3FF;
        frame_registers = registers;
    }
    if (first_spi < 32 || used >= spi_count || used >= MaximumMessages)
    {
        return false;
    }
    *interrupt_id = first_spi + used++;
    return true;
}

} // namespace
} // namespace FeatherOS::Arch::AArch64::Gicv2m

namespace FeatherOS::Arch::Msi
{

bool Allocate(Handler handler, void* context, FeatherOS::Pci::MsiMessage* message)
{
    using namespace AArch64;
    const PciDeviceTree::HostBridge* bridge = PciEcam::Bridge();
    if (handler == nullptr || message == nullptr || bridge == nullptr || !bridge->has_msi_frame ||
        bridge->msi_frame_size < Gicv2m::FrameSize)
    {
        return false;
    }
    uint32_t interrupt_id;
    {
        Sync::SpinLockGuard guard(Gicv2m::lock);
        if (!Gicv2m::Reserve(*bridge, &interrupt_id))
        {
            return false;
        }
    }
    // A pulse that arrives while the interrupt is still level-configured can be lost.
    ConfigureEdge(interrupt_id);
    RegisterInterruptHandler(interrupt_id, handler, context);
    message->address = bridge->msi_frame_base + Gicv2m::SetSpiRegister;
    message->data = interrupt_id;
    return true;
}

bool RegisterIntx(uint32_t irq, Handler handler, void* context)
{
    using namespace AArch64;
    if (handler == nullptr || irq < 32 || HasInterruptHandler(irq))
    {
        return false;
    }
    ConfigureLevel(irq);
    RegisterInterruptHandler(irq, handler, context);
    return true;
}

} // namespace FeatherOS::Arch::Msi
