#ifndef FEATHEROS_USB_HUB_H
#define FEATHEROS_USB_HUB_H

#include "kernel/usb/device.h"
#include "kernel/usb/driver.h"
#include "kernel/usb/hub_state.h"

#include <atomic>
#include <stdint.h>

namespace FeatherOS::Usb
{

extern const Driver HubDriver;

// The driver state of one hub interface. The manager task alone uses the control requests and
// the port states; the event task's completion only sets `pending` and posts the hub.
class Hub
{
public:
    Hub(Device& hub, bool super_speed);
    Hub(const Hub&) = delete;
    Hub& operator=(const Hub&) = delete;

    bool Allocate();
    void Release();

    Status GetPortStatus(uint32_t port, uint16_t* status, uint16_t* change);
    Status SetPortFeature(uint32_t port, uint8_t feature);
    Status ClearPortFeature(uint32_t port, uint8_t feature);
    Status GetHubStatus(uint16_t* status, uint16_t* change);
    Status ClearHubFeature(uint8_t feature);
    bool Listen();
    uint32_t TakePending();
    void Post(uint32_t bits);

    Device& device;
    const bool super_speed;
    bool multi_tt = false;
    uint8_t interface = 0;
    // The ports in use: at most HubPort::MaxPorts, the route string's limit.
    uint8_t ports = 0;
    HubPort::State port[HubPort::MaxPorts + 1];
    DeviceTransfer transfer;
    Memory::Dma::Buffer bitmap{};
    Memory::Dma::Buffer control{};

private:
    Status Request(uint8_t type, uint8_t request, uint16_t value, uint16_t index, uint16_t length);

    std::atomic<uint32_t> pending_ = 0;
    std::atomic<bool> queued_ = false;
};

Hub* HubOf(Device& device);

} // namespace FeatherOS::Usb

#endif
