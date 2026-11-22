#ifndef FEATHEROS_USB_MANAGER_H
#define FEATHEROS_USB_MANAGER_H

#include "kernel/usb/descriptors.h"
#include "kernel/usb/device.h"
#include "kernel/usb/driver.h"
#include "kernel/usb/hub.h"
#include "kernel/usb/host_controller.h"

#include <atomic>
#include <span>
#include <stdint.h>

namespace FeatherOS::Usb
{

constexpr uint32_t UnbindRecoveries = 3;

// The manager task's work for one controller: root-port changes, enumeration and removal. It is
// the only caller of the blocking HostController calls, so one device at a time is in the
// default state.
class Manager
{
public:
    explicit Manager(HostController& host, uint32_t bus = 1,
                     std::span<const Driver* const> drivers = BuiltInDrivers());

    bool HasWork() const;
    void Run();
    // The attached device on a slot; null when the slot has none.
    const Device* Find(uint32_t slot) const;
    uint32_t SlotOfPort(uint32_t port) const;

private:
    enum class Step : uint8_t;

    void HandlePort(uint32_t port);
    void HandleHub(const DeviceWork& work);
    void ServiceHubPort(Device& hub_device, Hub& hub, uint32_t port);
    void ServiceHubStatus(Hub& hub);
    bool ReadPortStatus(Hub& hub, uint32_t port, HubPort::Event* event);
    Device* Enumerate(const Location& location, Device* parent, uint8_t port);
    bool ReadDescriptors(Device& info, Memory::Dma::Buffer& buffer, Step* failed);
    Status GetDescriptor(uint32_t slot, uint8_t type, uint8_t index, uint16_t language,
                         uint16_t length, Memory::Dma::Buffer& buffer, uint32_t* actual);
    void Forget();
    void Service();
    void Log(const char* path, const char* message, bool warning) const;
    static void OnRecovery(void* context, uint32_t slot, uint8_t endpoint, uint32_t recent,
                           Status status);

    HostController& host_;
    Bus bus_;
    uint32_t port_slot_[MaxRootPorts + 1] = {};
    bool port_failed_[MaxRootPorts + 1] = {};
    std::atomic<bool> unbind_[MaxDeviceSlots + 1] = {};
    uint8_t configuration_bytes_[MaxConfigurationBytes] = {};
};

} // namespace FeatherOS::Usb

#endif
