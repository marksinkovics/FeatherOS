#include "probe.h"

#include "kernel/boot_options.h"
#include "kernel/klogger.h"
#include "kernel/power.h"
#include "kernel/storage/controllers.h"
#include "kernel/storage/kernel_environment.h"
#include "kernel/storage/nvme/controller.h"
#include "kernel/storage/pci_hardware.h"
#include "kernel/storage/probe_wait.h"
#include "kernel/storage/worker.h"
#include "kernel/storage/write_policy.h"
#include "kernel/string/format.h"

#include <atomic>
#include <stdint.h>

namespace FeatherOS::Storage::Nvme
{
namespace
{

constexpr uint32_t RegisterBar = 0;

constinit KLogger logger;
constinit std::atomic<uint16_t> next_index = 0;

void StartJob(void* context)
{
    static_cast<Controller*>(context)->Start();
    ProbeWait::End(KernelEnvironment(), context);
}

void ShutdownController(void* context, Power::Action)
{
    static_cast<Controller*>(context)->Shutdown();
}

void Say(uint16_t index, const char* text)
{
    char line[96];
    if (String::FormatTo(line, sizeof(line), "Storage: nvme{}: {}", index, text))
    {
        logger.Warning(line);
    }
}

} // namespace

bool Probe(Pci::ConfigSpace& config, const Pci::Function& function)
{
    const uint16_t index = next_index.fetch_add(1, std::memory_order_relaxed);
    PciHardware* hardware = PciHardware::Create(config, function, RegisterBar);
    if (hardware == nullptr)
    {
        Say(index, "cannot map BAR 0");
        return false;
    }
    char location[MaxDiskLocation];
    FormatPciLocation(function.address, location);
    const WritePolicy policy = DecideWrites(BootOptions::SelectedStorage(),
                                            hardware->SubsystemVendor(), hardware->Subsystem());
    Controller* controller =
        Controller::Create(*hardware, KernelEnvironment(), index, policy, location);
    if (controller == nullptr)
    {
        Say(index, "out of memory");
        return false;
    }
    if (!AddShutdown(ShutdownController, controller))
    {
        Say(index, "too many controllers to shut down");
    }
    char name[ProbeWait::NameCapacity];
    if (!String::FormatTo(name, sizeof(name), "nvme{}", index) ||
        !ProbeWait::Begin(controller, name))
    {
        Say(index, "too many controllers to wait for");
        return false;
    }
    if (!Worker::Post(StartJob, controller))
    {
        ProbeWait::End(KernelEnvironment(), controller);
        Say(index, "the storage worker's queue is full");
        return false;
    }
    return true;
}

} // namespace FeatherOS::Storage::Nvme
