#ifndef FEATHEROS_USB_STORAGE_DRIVER_H
#define FEATHEROS_USB_STORAGE_DRIVER_H

#include "kernel/usb/driver.h"
#include "kernel/storage/environment.h"

namespace FeatherOS::Usb::Storage
{

using Environment = FeatherOS::Storage::Environment;

// Bulk-only SCSI interfaces: class 08, subclass 06, protocol 50.
extern const Driver MassStorageDriver;

// Where the drives wait and sleep; none set, no drive attaches.
void SetEnvironment(Environment* environment);

} // namespace FeatherOS::Usb::Storage

#endif
