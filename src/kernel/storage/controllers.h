#ifndef FEATHEROS_STORAGE_CONTROLLERS_H
#define FEATHEROS_STORAGE_CONTROLLERS_H

#include "kernel/power.h"

namespace FeatherOS::Storage
{

using ControllerShutdown = void (*)(void* context, Power::Action action);

// Sets the kernel Environment, starts the storage worker and walks PCI for disk controllers.
// True when the worker runs.
bool StartControllers(const void* platform_data);
bool AddShutdown(ControllerShutdown shutdown, void* context);
void ShutdownControllers(Power::Action action);

} // namespace FeatherOS::Storage

#endif
