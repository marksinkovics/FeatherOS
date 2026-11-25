#ifndef FEATHEROS_STORAGE_KERNEL_ENVIRONMENT_H
#define FEATHEROS_STORAGE_KERNEL_ENVIRONMENT_H

#include "kernel/storage/environment.h"

namespace FeatherOS::Storage
{

// Waits and sleeps with the scheduler.
Environment& KernelEnvironment();

} // namespace FeatherOS::Storage

#endif
