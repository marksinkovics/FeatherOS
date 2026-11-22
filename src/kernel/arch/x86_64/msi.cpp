#include "msi.h"

#include "local_apic.h"

#include "kernel/arch/msi.h"
#include "kernel/sync/spinlock.h"

namespace FeatherOS::Arch::X86_64::Msi
{
namespace
{

constexpr uint64_t MessageBase = 0xFEE00000;

struct Slot
{
    FeatherOS::Arch::Msi::Handler handler;
    void* context;
};

constinit Sync::SpinLock lock;
Slot slots[LocalApic::MsiVectorCount] GUARDED_BY(lock) = {};
uint32_t used GUARDED_BY(lock) = 0;

} // namespace

void HandleInterrupt(uint64_t vector)
{
    LocalApic::EndOfInterrupt();
    Slot slot;
    {
        Sync::SpinLockGuard guard(lock);
        slot = slots[vector - LocalApic::FirstMsiVector];
    }
    if (slot.handler != nullptr)
    {
        slot.handler(slot.context);
    }
}

} // namespace FeatherOS::Arch::X86_64::Msi

namespace FeatherOS::Arch::Msi
{

bool Allocate(Handler handler, void* context, FeatherOS::Pci::MsiMessage* message)
{
    uint32_t apic_id;
    if (handler == nullptr || message == nullptr || !X86_64::LocalApic::InitializeBootCpu() ||
        !X86_64::LocalApic::BootCpuId(&apic_id))
    {
        return false;
    }
    uint32_t index;
    {
        Sync::SpinLockGuard guard(X86_64::Msi::lock);
        if (X86_64::Msi::used == X86_64::LocalApic::MsiVectorCount)
        {
            return false;
        }
        index = X86_64::Msi::used++;
        X86_64::Msi::slots[index] = {handler, context};
    }
    message->address = X86_64::Msi::MessageBase | (static_cast<uint64_t>(apic_id) << 12);
    message->data = X86_64::LocalApic::FirstMsiVector + index;
    return true;
}

bool RegisterIntx(uint32_t, Handler, void*)
{
    return false;
}

} // namespace FeatherOS::Arch::Msi
