#ifndef FEATHEROS_USB_HOST_CONTROLLER_H
#define FEATHEROS_USB_HOST_CONTROLLER_H

#include "kernel/memory/dma.h"

#include <atomic>
#include <span>
#include <stdint.h>

namespace FeatherOS::Usb
{

constexpr uint32_t MaxRootPorts = 255;
constexpr uint32_t MaxDeviceSlots = 64;
constexpr uint32_t MaxBulkTransferBytes = 65536;

enum class Speed : uint8_t
{
    Low = 1,
    Full,
    High,
    Super,
    SuperPlus,
};

// root_port is the controller's port number, counting from 1; a route holds one nibble per hub
// tier, the port of the hub on the root port in the lowest nibble. depth is the number of hubs
// above the device, so a hub on a root port has depth 0.
struct Location
{
    Speed speed = Speed::Full;
    uint8_t root_port;
    uint8_t depth;
    uint8_t tt_port;
    uint32_t route;
    uint32_t tt_hub_slot;
    bool tt_multi;
};

enum class EndpointType : uint8_t
{
    Control,
    Isochronous,
    Bulk,
    Interrupt,
};

// max_packet_size is wMaxPacketSize as read; the SuperSpeed fields come from the companion
// descriptor and stay 0 for other speeds.
struct EndpointDescriptor
{
    uint8_t address;
    EndpointType type;
    uint16_t max_packet_size;
    uint8_t interval;
    uint8_t max_burst;
    uint8_t mult;
    uint32_t bytes_per_interval;
};

struct HubInfo
{
    uint8_t ports;
    bool multi_tt;
    uint8_t think_time;
};

struct SetupPacket
{
    uint8_t request_type;
    uint8_t request;
    uint16_t value;
    uint16_t index;
    uint16_t length;
};

enum class Status : uint8_t
{
    Success,
    ShortPacket,
    Stall,
    TransactionError,
    Babble,
    Timeout,
    NoDevice,
    Cancelled,
    ControllerError,
};

constexpr bool Succeeded(Status status)
{
    return status == Status::Success || status == Status::ShortPacket;
}

// A transfer on a non-control endpoint. The object must stay alive and untouched from Submit
// until `finished` is set (a callback may resubmit it); done runs from the controller's event
// task after `finished` is set, so a transfer with a callback is never waited for.
struct Transfer
{
    uint8_t endpoint = 0;
    Memory::Dma::Buffer* buffer = nullptr;
    uint32_t length = 0;
    void (*done)(Transfer&) = nullptr;
    void* context = nullptr;
    Status status = Status::Success;
    uint32_t actual = 0;
    std::atomic<bool> finished = false;
    uint32_t slot = 0;
};

struct RootPortStatus
{
    bool connected;
    bool enabled;
    bool connect_changed;
    bool changed;
    bool speed_known;
    Speed speed = Speed::Full;
};

enum class CreateStatus : uint8_t
{
    Created,
    NoSlots,
    SlotFailed,
    AddressFailed,
    Unavailable,
};

// Called from the manager task after an endpoint was recovered from an error, with how many
// recoveries that endpoint had in the last second and the error that was recovered.
using RecoveryHandler = void (*)(void* context, uint32_t slot, uint8_t endpoint,
                                 uint32_t recent_recoveries, Status status);

// The USB core's view of one controller. Control, ConfigureEndpoints, ResetEndpoint,
// CancelEndpoint, ClearHalt, StopEndpoints, DestroyDevice, Wait and ServiceEndpoints block and
// belong to the manager task; Submit may be called from any task, including a transfer's own
// completion callback.
class HostController
{
public:
    virtual uint32_t RootPorts() const = 0;
    // With acknowledge, the change bits that were read are cleared in the port.
    virtual void ReadRootPort(uint32_t port, RootPortStatus* status, bool acknowledge) = 0;
    virtual bool ResetRootPort(uint32_t port, Speed* speed) = 0;

    virtual CreateStatus CreateDevice(const Location& location, uint32_t* slot) = 0;
    virtual bool SetControlMaxPacket(uint32_t slot, uint32_t max_packet) = 0;
    virtual bool ConfigureEndpoints(uint32_t slot, std::span<const EndpointDescriptor> endpoints,
                                    const HubInfo* hub) = 0;
    virtual void DestroyDevice(uint32_t slot) = 0;

    // The data stage uses the buffer, which must be a controller-allocated buffer of at least
    // setup.length bytes; actual gets the bytes transferred.
    virtual Status Control(uint32_t slot, const SetupPacket& setup, Memory::Dma::Buffer& buffer,
                           uint32_t* actual) = 0;
    // False when the transfer was not queued; its status then says why.
    virtual bool Submit(uint32_t slot, Transfer& transfer) = 0;
    virtual bool Wait(Transfer& transfer, uint64_t deadline_milliseconds) = 0;
    virtual bool ResetEndpoint(uint32_t slot, uint8_t endpoint) = 0;
    // Every transfer pending on the endpoint finishes Cancelled before this returns, and the
    // endpoint takes new transfers afterwards.
    virtual bool CancelEndpoint(uint32_t slot, uint8_t endpoint) = 0;
    // Resets the data toggle on the controller and on the device, so it also cancels what is
    // pending on the endpoint.
    virtual bool ClearHalt(uint32_t slot, uint8_t endpoint) = 0;
    virtual void StopEndpoints(uint32_t slot) = 0;

    virtual bool AllocateBuffer(uint64_t bytes, Memory::Dma::Buffer* buffer) = 0;
    virtual void FreeBuffer(const Memory::Dma::Buffer& buffer) = 0;
    virtual void Delay(uint32_t milliseconds) = 0;
    virtual uint64_t NowMilliseconds() = 0;

    virtual bool HasWork() const = 0;
    virtual bool TakePortChange(uint32_t* port) = 0;
    // Restarts the controller after an error; true when every device was detached by it.
    virtual bool Recover() = 0;
    virtual void ServiceEndpoints() = 0;
    virtual void SetRecoveryHandler(RecoveryHandler handler, void* context) = 0;

protected:
    HostController() = default;
    ~HostController() = default;
    HostController(const HostController&) = default;
    HostController& operator=(const HostController&) = default;
};

} // namespace FeatherOS::Usb

#endif
