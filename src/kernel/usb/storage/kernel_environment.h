#ifndef FEATHEROS_USB_STORAGE_KERNEL_ENVIRONMENT_H
#define FEATHEROS_USB_STORAGE_KERNEL_ENVIRONMENT_H

#include "kernel/usb/storage/environment.h"

namespace FeatherOS::Usb::Storage
{

// Waits and sleeps with the scheduler.
Environment& KernelEnvironment();

} // namespace FeatherOS::Usb::Storage

#endif
