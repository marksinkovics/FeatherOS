#ifndef FEATHEROS_USB_DRIVER_H
#define FEATHEROS_USB_DRIVER_H

#include "kernel/usb/descriptors.h"
#include "kernel/usb/host_controller.h"

#include <span>

namespace FeatherOS::Usb
{

struct Device;

// attach runs once the device is configured, and may submit transfers; detach runs after every
// transfer of the device has completed and no callback can run again, so it frees the state.
struct Driver
{
    const char* name;
    bool (*matches)(const InterfaceView& interface);
    Status (*attach)(Device& device, const InterfaceView& interface, void** state);
    void (*detach)(Device& device, void* state);
};

std::span<const Driver* const> BuiltInDrivers();

} // namespace FeatherOS::Usb

#endif
