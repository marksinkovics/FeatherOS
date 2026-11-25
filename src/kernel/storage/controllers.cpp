#include "controllers.h"

#include "kernel/arch/pci.h"
#include "kernel/klogger.h"
#include "kernel/pci/pci.h"
#include "kernel/storage/ahci/probe.h"
#include "kernel/storage/kernel_environment.h"
#include "kernel/storage/nvme/probe.h"
#include "kernel/storage/probe_wait.h"
#include "kernel/storage/worker.h"
#include "kernel/string/format.h"
#include "kernel/sync/spinlock.h"
#include "kernel/time/timer.h"
#include "kernel/usb/storage/driver.h"
#include "user/thread_safety.h"

#include <stddef.h>
#include <stdint.h>

namespace FeatherOS::Storage
{
namespace
{

constexpr uint32_t MaxShutdowns = 16;
constexpr uint8_t MassStorageClass = 0x01;
constexpr uint8_t SataSubclass = 0x06;
constexpr uint8_t RaidSubclass = 0x04;
constexpr uint8_t NvmSubclass = 0x08;
constexpr uint8_t AhciInterface = 0x01;
constexpr uint8_t NvmeInterface = 0x02;

struct Shutdown
{
    ControllerShutdown function;
    void* context;
};

constinit KLogger logger;
constinit Sync::SpinLock shutdown_lock;
Shutdown shutdowns[MaxShutdowns] GUARDED_BY(shutdown_lock) = {};
uint32_t shutdown_count GUARDED_BY(shutdown_lock) = 0;

void LogRaidMode(const Pci::Function& function)
{
    constexpr char Digits[] = "0123456789abcdef";
    char line[128];
    size_t length = 0;
    auto put = [&](const char* text)
    {
        for (; *text != '\0'; ++text)
        {
            line[length++] = *text;
        }
    };
    auto hex = [&](uint8_t value)
    {
        line[length++] = Digits[value >> 4];
        line[length++] = Digits[value & 0xF];
    };
    put("Storage: PCI ");
    hex(function.address.bus);
    put(":");
    hex(function.address.device);
    put(".");
    line[length++] = Digits[function.address.function & 0xF];
    put(" is a RAID-mode SATA controller, not supported; set the firmware's SATA mode to AHCI");
    line[length] = '\0';
    logger.Warning(line);
}

bool Visit(const Pci::Function& function, void* context)
{
    if (function.class_code != MassStorageClass)
    {
        return true;
    }
    if (function.subclass == NvmSubclass && function.prog_if == NvmeInterface)
    {
        Nvme::Probe(*static_cast<Pci::ConfigSpace*>(context), function);
        return true;
    }
    if (function.subclass == SataSubclass && function.prog_if == AhciInterface)
    {
        Ahci::Probe(*static_cast<Pci::ConfigSpace*>(context), function);
        return true;
    }
    if (function.subclass == RaidSubclass)
    {
        LogRaidMode(function);
    }
    return true;
}

} // namespace

bool StartControllers(const void* platform_data)
{
    if (!ProbeWait::MayWait(Time::TimerRunning()))
    {
        return false;
    }
    Usb::Storage::SetEnvironment(&KernelEnvironment());
    const bool started = Worker::Start();
    if (!started)
    {
        logger.Error("Storage: could not start the storage worker");
    }
    Pci::ConfigSpace* config = Arch::Pci::Initialize(platform_data);
    if (config != nullptr)
    {
        Pci::Enumerate(*config, Visit, config);
    }
    return started;
}

bool WaitForControllers(uint64_t timeout_ms)
{
    Environment& environment = KernelEnvironment();
    char late[ProbeWait::MaxPending * ProbeWait::NameCapacity];
    if (ProbeWait::Wait(environment, environment.NowMilliseconds() + timeout_ms, late,
                        sizeof(late)))
    {
        return true;
    }
    char line[sizeof(late) + 64];
    if (String::FormatTo(line, sizeof(line),
                         "Storage: {} still probing; the root is chosen without it", late))
    {
        logger.Warning(line);
    }
    return false;
}

bool AddShutdown(ControllerShutdown shutdown, void* context)
{
    Sync::SpinLockGuard guard(shutdown_lock);
    if (shutdown == nullptr || shutdown_count == MaxShutdowns)
    {
        return false;
    }
    shutdowns[shutdown_count++] = {shutdown, context};
    return true;
}

void ShutdownControllers(Power::Action action)
{
    Shutdown copy[MaxShutdowns];
    uint32_t count = 0;
    {
        Sync::SpinLockGuard guard(shutdown_lock);
        count = shutdown_count;
        for (uint32_t index = 0; index < count; ++index)
        {
            copy[index] = shutdowns[index];
        }
    }
    for (uint32_t index = 0; index < count; ++index)
    {
        copy[index].function(copy[index].context, action);
    }
}

} // namespace FeatherOS::Storage
