#include "power.h"

#include "kernel/arch/interrupts.h"
#include "kernel/arch/power.h"
#include "kernel/console.h"
#include "kernel/files.h"
#include "kernel/panic.h"
#include "kernel/process.h"
#include "kernel/smp.h"
#include "kernel/storage/block_device.h"

namespace FeatherOS::Power
{

void Shutdown(Action action, uint64_t caller_process)
{
    Console::WriteLine("KERN/I: Shutdown: stopping processes");
    if (!Process::StopOtherProcesses(caller_process))
    {
        Console::WriteLine("KERN/W: Shutdown: some processes did not stop");
    }
    // No filesystem call can start after this, and none is half done.
    Files::Quiesce();
    Console::WriteLine(BlockDevice::Flush() ? "KERN/I: Shutdown: disk flushed"
                                            : "KERN/W: Shutdown: disk flush failed");
    Console::WriteLine(action == Action::PowerOff ? "KERN/I: Powering off" : "KERN/I: Restarting");
    // Nothing may print after this point: a stopped CPU could be holding the console's locks.
    const bool stopped = Smp::StopOtherCpus();
    Arch::Interrupts::SaveAndDisable();
    if (stopped)
    {
        if (action == Action::PowerOff)
        {
            Arch::Power::PowerOff();
        }
        else
        {
            Arch::Power::Reset();
        }
    }
    Panic(action == Action::PowerOff ? "The firmware did not power off"
                                     : "The firmware did not restart");
}

} // namespace FeatherOS::Power
