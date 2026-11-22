#ifndef FEATHEROS_USB_DEVICE_H
#define FEATHEROS_USB_DEVICE_H

#include "kernel/sync/mutex.h"
#include "kernel/usb/descriptors.h"
#include "kernel/usb/driver.h"
#include "kernel/usb/host_controller.h"
#include "user/thread_safety.h"

#include <atomic>
#include <span>
#include <stddef.h>
#include <stdint.h>

namespace FeatherOS::Usb
{

constexpr uint32_t MaxProductName = 48;
constexpr uint32_t MaxPathName = 24;
constexpr uint32_t RootHubNumber = 1;
constexpr uint32_t MaxDeviceNumber = 127;
constexpr uint32_t MaxBuses = 4;

// Lock order: the /proc volume lock, then topology_lock, then the controller's ring_lock and the
// spinlocks. The event task never takes it.
extern Sync::Mutex topology_lock;

struct Device;

// A transfer a driver submits through its Device, so the core can count it and drop its
// completion once the device was detached. callback runs from the controller's event task.
struct DeviceTransfer : Transfer
{
    void (*callback)(DeviceTransfer&) = nullptr;
    void* driver_context = nullptr;
    Device* device = nullptr;
    uint32_t generation = 0;
};

// The fields up to `product` are written by the manager task before the device is attached and
// stay as they are until it is detached; the rest changes under topology_lock.
struct Device
{
    Device() = default;
    Device(const Device&) = delete;
    Device& operator=(const Device&) = delete;

    bool Submit(DeviceTransfer& transfer);
    HostController& Host() const;
    void Reset(HostController& host, uint32_t bus_number, uint32_t slot_number,
               const Location& where);

    uint32_t bus = 0;
    uint32_t slot = 0;
    Location location{};
    DeviceDescriptor device{};
    ConfigurationView configuration{};
    char product[MaxProductName] = {};

    uint32_t number = 0;
    char path[MaxPathName] = {};
    Device* parent = nullptr;
    uint8_t parent_port = 0;
    bool attached = false;
    const Driver* drivers[MaxInterfaces] = {};
    void* driver_state[MaxInterfaces] = {};

    std::atomic<uint32_t> generation = 0;
    std::atomic<uint32_t> in_flight = 0;
    std::atomic<bool> detaching = false;

private:
    HostController* host_ = nullptr;
};

// The device tree of one controller (bus). Only the manager task changes it; /proc/usb reads it
// under topology_lock.
class Bus
{
public:
    Bus(HostController& host, uint32_t number, std::span<const Driver* const> drivers);
    ~Bus();
    Bus(const Bus&) = delete;
    Bus& operator=(const Bus&) = delete;

    uint32_t Number() const;
    Device& Prepare(uint32_t slot, const Location& location);
    void Attach(Device& device, Device* parent, uint8_t parent_port);
    void Detach(Device& device);
    void UnbindDrivers(Device& device);
    void DropAll();
    Device* Find(uint32_t slot);
    const Device* Find(uint32_t slot) const;
    uint32_t Count() const;

private:
    friend size_t FormatDevices(char* buffer, size_t capacity);

    Device* ChildOf(const Device& parent);
    uint32_t AllocateNumber() REQUIRES(topology_lock);
    const char* Bind(Device& device) REQUIRES(topology_lock);
    bool Quiesce(Device& device) REQUIRES(topology_lock);
    void Unbind(Device& device, bool stuck) REQUIRES(topology_lock);
    void LogLine(const char* path, const char* message, bool warning);

    HostController& host_;
    uint32_t number_;
    std::span<const Driver* const> drivers_;
    Device devices_[MaxDeviceSlots + 1];
    uint32_t used_[(MaxDeviceNumber + 32) / 32] GUARDED_BY(topology_lock) = {};
};

// One line per attached device, in bus and number order, as /proc/usb shows them; whole lines
// only. Takes topology_lock. Returns the text's length.
size_t FormatDevices(char* buffer, size_t capacity);

} // namespace FeatherOS::Usb

#endif
