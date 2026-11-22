#ifndef FEATHEROS_USB_MANAGER_H
#define FEATHEROS_USB_MANAGER_H

#include "kernel/usb/descriptors.h"
#include "kernel/usb/host_controller.h"

#include <stdint.h>

namespace FeatherOS::Usb
{

constexpr uint32_t MaxProductName = 48;

struct DeviceInfo
{
    uint32_t slot;
    Location location;
    DeviceDescriptor device;
    ConfigurationView configuration;
    char product[MaxProductName];
};

// The manager task's work for one controller: root-port changes, enumeration and removal. It is
// the only caller of the blocking HostController calls, so one device at a time is in the
// default state.
class Manager
{
public:
    explicit Manager(HostController& host);

    bool HasWork() const;
    void Run();
    // The descriptors read from the device on a slot; null when the slot has no device.
    const DeviceInfo* Find(uint32_t slot) const;
    uint32_t SlotOfPort(uint32_t port) const;

private:
    enum class Step : uint8_t;

    void HandlePort(uint32_t port);
    bool Enumerate(uint32_t port);
    bool ReadDescriptors(DeviceInfo& info, Memory::Dma::Buffer& buffer, Step* failed);
    Status GetDescriptor(uint32_t slot, uint8_t type, uint8_t index, uint16_t language,
                         uint16_t length, Memory::Dma::Buffer& buffer, uint32_t* actual);
    void Forget();

    HostController& host_;
    uint32_t port_slot_[MaxRootPorts + 1] = {};
    bool port_failed_[MaxRootPorts + 1] = {};
    bool known_[MaxDeviceSlots + 1] = {};
    DeviceInfo devices_[MaxDeviceSlots + 1];
    uint8_t configuration_bytes_[MaxConfigurationBytes] = {};
};

} // namespace FeatherOS::Usb

#endif
