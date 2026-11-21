#include "kernel/arch/msi.h"
#include "kernel/arch/pci.h"
#include "kernel/boot_options.h"
#include "kernel/memory/device_memory.h"
#include "kernel/memory/heap.h"
#include "kernel/pci/msi.h"
#include "kernel/scheduler.h"
#include "kernel/sync/spinlock.h"
#include "kernel/time/timer.h"
#include "kernel/usb/xhci/controller.h"

#include <new>

namespace FeatherOS::Usb::Xhci
{
namespace
{

constexpr uint32_t MsixTableRegister = 4;
constexpr uint32_t MsixEntryBytes = 16;

uint64_t TicksFor(uint64_t milliseconds)
{
    return (milliseconds * Time::TicksPerSecond() + 999) / 1000;
}

class PciHardware final : public Hardware
{
public:
    PciHardware(Pci::ConfigSpace& config, const Pci::Function& function)
        : config_(config), function_(function)
    {
    }

    bool Map()
    {
        Pci::Bar bar;
        if (!Pci::ReadBar(config_, function_, 0, &bar) || bar.is_io || bar.address == 0)
        {
            return false;
        }
        base_ = Memory::MapDevice(bar.address, bar.size);
        if (base_ == 0)
        {
            return false;
        }
        Pci::EnableDevice(config_, function_, false);
        return true;
    }

    uint32_t Read32(uint32_t offset) override
    {
        return *reinterpret_cast<volatile uint32_t*>(base_ + offset);
    }

    void Write32(uint32_t offset, uint32_t value) override
    {
        *reinterpret_cast<volatile uint32_t*>(base_ + offset) = value;
    }

    void Write64(uint32_t offset, uint64_t value) override
    {
        Write32(offset, static_cast<uint32_t>(value));
        Write32(offset + 4, static_cast<uint32_t>(value >> 32));
    }

    uint32_t ReadConfig32(uint16_t offset) override
    {
        return config_.Read32(function_.address, offset);
    }

    void WriteConfig32(uint16_t offset, uint32_t value) override
    {
        config_.Write32(function_.address, offset, value);
    }

    bool AllocateDma(uint64_t bytes, uint64_t below, Memory::Dma::Buffer* buffer) override
    {
        return Memory::Dma::Allocate(bytes, below, buffer);
    }

    void FreeDma(const Memory::Dma::Buffer& buffer) override
    {
        Memory::Dma::Free(buffer);
    }

    uint64_t NowMilliseconds() override
    {
        return Time::TickCount() * 1000 / Time::TicksPerSecond();
    }

    void Pause(uint32_t milliseconds) override
    {
        Scheduler::SleepUntil(Time::TickCount() + TicksFor(milliseconds));
    }

    bool WaitUntil(bool (*done)(const void* context), const void* context,
                   uint64_t deadline_milliseconds) override
    {
        const uint64_t deadline_tick = TicksFor(deadline_milliseconds);
        Sync::SpinLockGuard guard(completion_lock_);
        for (;;)
        {
            if (done(context))
            {
                return true;
            }
            if (NowMilliseconds() >= deadline_milliseconds)
            {
                return false;
            }
            Scheduler::WaitOn(completion_wait_, completion_lock_, deadline_tick);
        }
    }

    void WakeWaiters() override
    {
        Sync::SpinLockGuard guard(completion_lock_);
        Scheduler::WakeAll(completion_wait_);
    }

    void SignalEvents() override
    {
        Sync::SpinLockGuard guard(event_lock_);
        event_pending_ = true;
        Scheduler::WakeAll(event_wait_);
    }

    void WaitForEvents(uint64_t deadline_milliseconds) override
    {
        const uint64_t deadline_tick = TicksFor(deadline_milliseconds);
        Sync::SpinLockGuard guard(event_lock_);
        while (!event_pending_ && NowMilliseconds() < deadline_milliseconds)
        {
            Scheduler::WaitOn(event_wait_, event_lock_, deadline_tick);
        }
        event_pending_ = false;
    }

    InterruptKind SetUpInterrupts(bool allow, void (*handler)(void* context),
                                  void* context) override
    {
        if (allow)
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
            uint32_t irq;
            if (Arch::Pci::RouteIntx(function_, &irq) &&
                Arch::Msi::RegisterIntx(irq, handler, context))
            {
                Pci::UpdateCommand(config_, function_, 0, Pci::CommandInterruptDisable);
                return InterruptKind::Intx;
            }
        }
        Pci::UpdateCommand(config_, function_, Pci::CommandInterruptDisable, 0);
        return InterruptKind::Polling;
    }

private:
    bool EnableMsiX(const Pci::MsiMessage& message)
    {
        const uint16_t capability = Pci::FindCapability(config_, function_, Pci::CapabilityMsix);
        if (capability == 0)
        {
            return false;
        }
        const uint32_t table = config_.Read32(function_.address, capability + MsixTableRegister);
        Pci::Bar bar;
        if (!Pci::ReadBar(config_, function_, table & 7U, &bar) || bar.is_io)
        {
            return false;
        }
        const uintptr_t mapped = Memory::MapDevice(bar.address + (table & ~7U), MsixEntryBytes);
        return mapped != 0 && Pci::EnableMsiX(config_, function_, mapped, 0, message);
    }

    Pci::ConfigSpace& config_;
    Pci::Function function_;
    uintptr_t base_ = 0;
    Sync::SpinLock completion_lock_;
    Scheduler::WaitQueue completion_wait_;
    Sync::SpinLock event_lock_;
    Scheduler::WaitQueue event_wait_;
    bool event_pending_ GUARDED_BY(event_lock_) = false;
};

} // namespace

Controller* Controller::Probe(Pci::ConfigSpace& config, const Pci::Function& function)
{
    void* hardware_memory = Memory::AllocateZeroed(1, sizeof(PciHardware));
    void* controller_memory = Memory::AllocateZeroed(1, sizeof(Controller));
    if (hardware_memory == nullptr || controller_memory == nullptr)
    {
        Memory::Deallocate(hardware_memory);
        Memory::Deallocate(controller_memory);
        return nullptr;
    }
    auto* hardware = new (hardware_memory) PciHardware(config, function);
    auto* controller = new (controller_memory) Controller(*hardware, function);
    if (hardware->Map() &&
        controller->Initialize(BootOptions::SelectedUsb() != BootOptions::UsbMode::Poll))
    {
        return controller;
    }
    if (controller->InterruptRegistered())
    {
        return nullptr;
    }
    controller->~Controller();
    hardware->~PciHardware();
    Memory::Deallocate(controller_memory);
    Memory::Deallocate(hardware_memory);
    return nullptr;
}

} // namespace FeatherOS::Usb::Xhci
