#ifndef FEATHEROS_STORAGE_CONTROLLERS_H
#define FEATHEROS_STORAGE_CONTROLLERS_H

#include "kernel/power.h"

#include <stdint.h>

namespace FeatherOS::Storage
{

using ControllerShutdown = void (*)(void* context, Power::Action action);

// Sets the kernel Environment, starts the storage worker and walks PCI for disk controllers.
// True when the worker runs. Refused, with a log line, while the timer is not running.
bool StartControllers(const void* platform_data);
// Blocks the calling task until every controller's bring-up has ended or timeout_ms has passed,
// so that the disks it registers exist; false after the timeout, with the late ones named. A
// late controller still registers its disks, which are then opened like any other disk's.
bool WaitForControllers(uint64_t timeout_ms);
bool AddShutdown(ControllerShutdown shutdown, void* context);
void ShutdownControllers(Power::Action action);

} // namespace FeatherOS::Storage

#endif
