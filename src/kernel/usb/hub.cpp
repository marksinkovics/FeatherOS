#include "hub.h"

#include "kernel/klogger.h"
#include "kernel/usb/descriptors.h"
#include "kernel/usb/text.h"

namespace FeatherOS::Usb
{
namespace
{

constexpr uint8_t HubClass = 9;
constexpr uint32_t BufferBytes = 128;
constexpr uint32_t BitmapBytes = 64;
constexpr uint32_t EndpointPacketMask = 0x7FF;
constexpr uint32_t DescriptorBytes = 71;
constexpr uint32_t SuperSpeedDescriptorBytes = 12;
constexpr uint8_t HubProtocolMultiTt = 2;
constexpr uint8_t ClassToDevice = 0x20;
constexpr uint8_t DeviceToHostClass = 0xA0;
constexpr uint8_t ClassToOther = 0x23;
constexpr uint8_t OtherToHostClass = 0xA3;
constexpr uint8_t ToInterface = 0x01;
constexpr uint8_t GetStatusRequest = 0;
constexpr uint8_t ClearFeatureRequest = 1;
constexpr uint8_t SetFeatureRequest = 3;
constexpr uint8_t GetDescriptorRequest = 6;
constexpr uint8_t SetInterfaceRequest = 11;
constexpr uint8_t SetHubDepthRequest = 12;
constexpr uint32_t PowerGoodUnitMs = 2;

constinit KLogger logger;

void LogHub(const Device& device, const char* message, bool warning)
{
    char line[96];
    Text text(line, sizeof(line));
    text.Words("USB ");
    text.Words(device.path);
    text.Words(": ");
    text.Words(message);
    if (text.Finish())
    {
        if (warning)
        {
            logger.Warning(line);
        }
        else
        {
            logger.Info(line);
        }
    }
}

const EndpointDescriptor* FindEndpoint(const InterfaceView& interface)
{
    for (uint32_t index = 0; index < interface.endpoint_count; ++index)
    {
        const EndpointDescriptor& endpoint = interface.endpoints[index];
        if (endpoint.type == EndpointType::Interrupt && (endpoint.address & 0x80U) != 0)
        {
            return &endpoint;
        }
    }
    return nullptr;
}

bool Matches(const InterfaceView& interface)
{
    return interface.class_code == HubClass && FindEndpoint(interface) != nullptr;
}

void OnChange(DeviceTransfer& transfer)
{
    auto* hub = static_cast<Hub*>(transfer.driver_context);
    const Status status = transfer.status;
    if (status == Status::Cancelled || status == Status::NoDevice)
    {
        return;
    }
    uint32_t bits = 0;
    if (Succeeded(status))
    {
        const uint32_t bytes = transfer.actual < 2 ? transfer.actual : 2;
        for (uint32_t index = 0; index < bytes; ++index)
        {
            bits |= static_cast<uint32_t>(hub->bitmap.address[index]) << (8 * index);
        }
        bits &= (2U << hub->ports) - 1;
    }
    if (bits == 0)
    {
        hub->device.Submit(transfer);
        return;
    }
    hub->Post(bits);
}

bool ReadDescriptor(Hub& hub, HubDescriptor* descriptor)
{
    const uint32_t length = hub.super_speed ? SuperSpeedDescriptorBytes : DescriptorBytes;
    const uint8_t type = hub.super_speed ? DescriptorType::SuperSpeedHub : DescriptorType::Hub;
    uint32_t actual = 0;
    const SetupPacket setup = {DeviceToHostClass, GetDescriptorRequest,
                               static_cast<uint16_t>(type << 8), 0, static_cast<uint16_t>(length)};
    if (hub.device.Host().Control(hub.device.slot, setup, hub.control, &actual) != Status::Success)
    {
        return false;
    }
    uint8_t bytes[DescriptorBytes];
    const uint32_t count = actual < DescriptorBytes ? actual : DescriptorBytes;
    for (uint32_t index = 0; index < count; ++index)
    {
        bytes[index] = hub.control.address[index];
    }
    return ParseHub(std::span<const uint8_t>(bytes, count), hub.super_speed, descriptor) ==
           ParseStatus::Ok;
}

Status Attach(Device& device, const InterfaceView& interface, void** state)
{
    const EndpointDescriptor* endpoint = FindEndpoint(interface);
    if (endpoint == nullptr)
    {
        return Status::Stall;
    }
    HostController& host = device.Host();
    auto* hub = new Hub(device, device.location.speed >= Speed::Super);
    hub->interface = interface.number;
    HubDescriptor descriptor{};
    bool ready = hub->Allocate() && ReadDescriptor(*hub, &descriptor);
    if (ready && hub->super_speed)
    {
        uint32_t actual = 0;
        const SetupPacket depth = {ClassToDevice, SetHubDepthRequest, device.location.depth, 0, 0};
        ready = host.Control(device.slot, depth, hub->control, &actual) == Status::Success;
    }
    if (!ready)
    {
        hub->Release();
        delete hub;
        return Status::Stall;
    }
    if (!hub->super_speed && device.device.device_protocol == HubProtocolMultiTt &&
        device.location.speed == Speed::High)
    {
        uint32_t actual = 0;
        const SetupPacket select = {ToInterface, SetInterfaceRequest, 1, interface.number, 0};
        hub->multi_tt = host.Control(device.slot, select, hub->control, &actual) == Status::Success;
    }
    const uint32_t usable =
        descriptor.ports < HubPort::MaxPorts ? descriptor.ports : HubPort::MaxPorts;
    hub->ports = static_cast<uint8_t>(usable);
    for (uint32_t index = 1; index <= usable; ++index)
    {
        hub->port[index].usb3 = hub->super_speed;
    }
    const HubInfo info = {descriptor.ports, hub->multi_tt, descriptor.think_time};
    if (!host.ConfigureEndpoints(device.slot, {}, &info))
    {
        hub->Release();
        delete hub;
        return Status::Stall;
    }
    if (descriptor.ports > HubPort::MaxPorts)
    {
        LogHub(device, "hub has more ports than a route string holds, the rest are unused", true);
    }
    uint32_t powered = 0;
    for (uint32_t index = 1; index <= usable; ++index)
    {
        if (hub->SetPortFeature(index, HubPort::Feature::PortPower) == Status::Success)
        {
            ++powered;
        }
    }
    if (powered == 0)
    {
        hub->Release();
        delete hub;
        return Status::Stall;
    }
    host.Delay(static_cast<uint32_t>(descriptor.power_on_to_good) * PowerGoodUnitMs);
    const uint32_t packet = endpoint->max_packet_size & EndpointPacketMask;
    hub->transfer.endpoint = endpoint->address;
    hub->transfer.buffer = &hub->bitmap;
    hub->transfer.length = packet == 0 ? 1 : (packet < BitmapBytes ? packet : BitmapBytes);
    hub->transfer.callback = OnChange;
    hub->transfer.driver_context = hub;
    if (!hub->Listen())
    {
        hub->Release();
        delete hub;
        return Status::Stall;
    }
    *state = hub;
    hub->Post((2U << usable) - 2);
    return Status::Success;
}

void Detach(Device& device, void* state)
{
    (void)device;
    auto* hub = static_cast<Hub*>(state);
    hub->Release();
    delete hub;
}

} // namespace

const Driver HubDriver = {"hub", Matches, Attach, Detach};

Hub::Hub(Device& hub, bool super) : device(hub), super_speed(super) {}

bool Hub::Allocate()
{
    HostController& host = device.Host();
    return host.AllocateBuffer(BufferBytes, &control) && host.AllocateBuffer(BitmapBytes, &bitmap);
}

void Hub::Release()
{
    HostController& host = device.Host();
    if (control.size != 0)
    {
        host.FreeBuffer(control);
        control = {};
    }
    if (bitmap.size != 0)
    {
        host.FreeBuffer(bitmap);
        bitmap = {};
    }
}

Status Hub::Request(uint8_t type, uint8_t request, uint16_t value, uint16_t index, uint16_t length)
{
    uint32_t actual = 0;
    const SetupPacket setup = {type, request, value, index, length};
    return device.Host().Control(device.slot, setup, control, &actual);
}

Status Hub::GetPortStatus(uint32_t port_number, uint16_t* status, uint16_t* change)
{
    const Status result =
        Request(OtherToHostClass, GetStatusRequest, 0, static_cast<uint16_t>(port_number), 4);
    if (result != Status::Success)
    {
        return result;
    }
    *status = static_cast<uint16_t>(control.address[0] | (control.address[1] << 8));
    *change = static_cast<uint16_t>(control.address[2] | (control.address[3] << 8));
    return result;
}

Status Hub::SetPortFeature(uint32_t port_number, uint8_t feature)
{
    return Request(ClassToOther, SetFeatureRequest, feature, static_cast<uint16_t>(port_number), 0);
}

Status Hub::ClearPortFeature(uint32_t port_number, uint8_t feature)
{
    return Request(ClassToOther, ClearFeatureRequest, feature, static_cast<uint16_t>(port_number),
                   0);
}

Status Hub::GetHubStatus(uint16_t* status, uint16_t* change)
{
    const Status result = Request(DeviceToHostClass, GetStatusRequest, 0, 0, 4);
    if (result != Status::Success)
    {
        return result;
    }
    *status = static_cast<uint16_t>(control.address[0] | (control.address[1] << 8));
    *change = static_cast<uint16_t>(control.address[2] | (control.address[3] << 8));
    return result;
}

Status Hub::ClearHubFeature(uint8_t feature)
{
    return Request(ClassToDevice, ClearFeatureRequest, feature, 0, 0);
}

bool Hub::Listen()
{
    return device.Submit(transfer);
}

uint32_t Hub::TakePending()
{
    const uint32_t bits = pending_.exchange(0);
    queued_.store(false);
    return bits;
}

void Hub::Post(uint32_t bits)
{
    pending_.fetch_or(bits);
    if (!queued_.exchange(true))
    {
        device.PostWork();
    }
}

Hub* HubOf(Device& device)
{
    for (uint32_t index = 0; index < MaxInterfaces; ++index)
    {
        if (device.drivers[index] == &HubDriver && device.driver_state[index] != nullptr)
        {
            return static_cast<Hub*>(device.driver_state[index]);
        }
    }
    return nullptr;
}

} // namespace FeatherOS::Usb
