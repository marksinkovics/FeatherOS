#include "pci_hardware.h"

#include "kernel/arch/msi.h"
#include "kernel/memory/device_memory.h"
#include "kernel/memory/dma.h"
#include "kernel/memory/heap.h"

#include <new>

namespace FeatherOS::Storage
{
namespace
{

constexpr uint16_t SubsystemRegister = 0x2C;
constexpr uint32_t MsixTableRegister = 4;
constexpr uint32_t MsixEntryBytes = 16;

} // namespace

PciHardware::PciHardware(Pci::ConfigSpace& config, const Pci::Function& function)
    : config_(config), function_(function)
{
}

PciHardware* PciHardware::Create(Pci::ConfigSpace& config, const Pci::Function& function,
                                 uint32_t register_bar)
{
    void* memory = Memory::AllocateZeroed(1, sizeof(PciHardware));
    if (memory == nullptr)
    {
        return nullptr;
    }
    auto* hardware = new (memory) PciHardware(config, function);
    if (!hardware->Map(register_bar))
    {
        hardware->~PciHardware();
        Memory::Deallocate(memory);
        return nullptr;
    }
    return hardware;
}

bool PciHardware::Map(uint32_t register_bar)
{
    Pci::Bar bar;
    if (!Pci::ReadBar(config_, function_, register_bar, &bar) || bar.is_io || bar.address == 0)
    {
        return false;
    }
    base_ = Memory::MapDevice(bar.address, bar.size);
    if (base_ == 0)
    {
        return false;
    }
    Pci::EnableDevice(config_, function_, true);
    return true;
}

uint16_t PciHardware::SubsystemVendor()
{
    return static_cast<uint16_t>(ReadConfig32(SubsystemRegister));
}

uint16_t PciHardware::Subsystem()
{
    return static_cast<uint16_t>(ReadConfig32(SubsystemRegister) >> 16);
}

uint32_t PciHardware::Read32(uint32_t offset)
{
    return *reinterpret_cast<volatile uint32_t*>(base_ + offset);
}

void PciHardware::Write32(uint32_t offset, uint32_t value)
{
    *reinterpret_cast<volatile uint32_t*>(base_ + offset) = value;
}

uint64_t PciHardware::Read64(uint32_t offset)
{
    const uint64_t low = Read32(offset);
    const uint64_t high = Read32(offset + 4);
    return low | (high << 32);
}

void PciHardware::Write64(uint32_t offset, uint64_t value)
{
    Write32(offset, static_cast<uint32_t>(value));
    Write32(offset + 4, static_cast<uint32_t>(value >> 32));
}

uint32_t PciHardware::ReadConfig32(uint16_t offset)
{
    return config_.Read32(function_.address, offset);
}

bool PciHardware::AllocateDma(uint64_t bytes, uint64_t below, Memory::Dma::Buffer* buffer)
{
    return Memory::Dma::Allocate(bytes, below, buffer);
}

void PciHardware::FreeDma(const Memory::Dma::Buffer& buffer)
{
    Memory::Dma::Free(buffer);
}

InterruptKind PciHardware::SetUpInterrupts(void (*handler)(void* context), void* context)
{
    Pci::MsiMessage message;
    if (Arch::Msi::Allocate(handler, context, &message))
    {
        if (EnableMsiX(message))
        {
            return InterruptKind::MsiX;
        }
        if (Pci::EnableMsi(config_, function_, message))
        {
            return InterruptKind::Msi;
        }
    }
    Pci::UpdateCommand(config_, function_, Pci::CommandInterruptDisable, 0);
    return InterruptKind::Polling;
}

bool PciHardware::EnableMsiX(const Pci::MsiMessage& message)
{
    const uint16_t capability = Pci::FindCapability(config_, function_, Pci::CapabilityMsix);
    if (capability == 0)
    {
        return false;
    }
    const uint32_t table = ReadConfig32(static_cast<uint16_t>(capability + MsixTableRegister));
    Pci::Bar bar;
    if (!Pci::ReadBar(config_, function_, table & 7U, &bar) || bar.is_io)
    {
        return false;
    }
    const uintptr_t mapped = Memory::MapDevice(bar.address + (table & ~7U), MsixEntryBytes);
    return mapped != 0 && Pci::EnableMsiX(config_, function_, mapped, 0, message);
}

} // namespace FeatherOS::Storage
