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
// service runs on the manager task, without topology_lock, after the driver called
// Device::PostWork; it may block in HostController calls and returns true to ask for a port
// reset. Stalls on the endpoints of a driver with stalls_are_protocol do not count towards
// unbinding it.
struct Driver
{
    const char* name;
    bool (*matches)(const InterfaceView& interface);
    Status (*attach)(Device& device, const InterfaceView& interface, void** state);
    void (*detach)(Device& device, void* state);
    bool (*service)(Device& device, void* state) = nullptr;
    bool stalls_are_protocol = false;
};

std::span<const Driver* const> BuiltInDrivers();

} // namespace FeatherOS::Usb

#endif
