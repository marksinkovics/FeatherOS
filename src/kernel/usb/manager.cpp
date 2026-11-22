#include "manager.h"

#include "kernel/klogger.h"
#include "kernel/usb/descriptors.h"
#include "kernel/usb/text.h"

namespace FeatherOS::Usb
{
namespace
{

constexpr uint32_t DebounceMs = HubPort::DebounceMilliseconds;
constexpr uint32_t ResetPollMs = 10;
constexpr uint32_t MaxPortRounds = 256;
constexpr uint32_t HubStatusChangeBits = 2;
constexpr uint8_t ClearLocalPower = 0;
constexpr uint8_t ClearHubOverCurrent = 1;
constexpr uint8_t DeviceToHostStandard = 0x80;
constexpr uint8_t GetDescriptorRequest = 6;
constexpr uint32_t ShortDeviceBytes = 8;
constexpr uint32_t LanguageBytes = 4;
constexpr uint32_t StringBytes = 128;

constinit KLogger logger;

uint32_t DefaultControlPacket(Speed speed)
{
    switch (speed)
    {
    case Speed::Low:
    case Speed::Full:
        return 8;
    case Speed::High:
        return 64;
    case Speed::Super:
    case Speed::SuperPlus:
        break;
    }
    return 512;
}

bool ValidControlPacket(Speed speed, uint32_t packet)
{
    switch (speed)
    {
    case Speed::Low:
        return packet == 8;
    case Speed::Full:
        return packet == 8 || packet == 16 || packet == 32 || packet == 64;
    case Speed::High:
        return packet == 64;
    case Speed::Super:
    case Speed::SuperPlus:
        break;
    }
    return packet == 512;
}

uint32_t PacketFromDescriptor(Speed speed, uint8_t value)
{
    if (speed >= Speed::Super)
    {
        return value == 9 ? 512 : 0;
    }
    return value;
}

} // namespace

enum class Manager::Step : uint8_t
{
    ShortDevice,
    ControlPacket,
    Device,
    ConfigurationHeader,
    ConfigurationSize,
    Configuration,
    ConfigurationParse,
    Gone,
};

Manager::Manager(HostController& host, uint32_t bus, std::span<const Driver* const> drivers)
    : host_(host), bus_(host, bus, drivers)
{
    host_.SetRecoveryHandler(OnRecovery, this);
}

void Manager::Log(const char* path, const char* message, bool warning) const
{
    char line[96];
    Text text(line, sizeof(line));
    text.Words("USB ");
    text.Words(path);
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

void Manager::OnRecovery(void* context, uint32_t slot, uint8_t endpoint, uint32_t recent, Status)
{
    auto* manager = static_cast<Manager*>(context);
    if (endpoint != 0 && recent >= UnbindRecoveries && slot <= MaxDeviceSlots)
    {
        manager->unbind_[slot].store(true);
    }
}

bool Manager::HasWork() const
{
    return host_.HasWork() || bus_.HasWork();
}

const Device* Manager::Find(uint32_t slot) const
{
    return bus_.Find(slot);
}

uint32_t Manager::SlotOfPort(uint32_t port) const
{
    return port <= MaxRootPorts ? port_slot_[port] : 0;
}

void Manager::Forget()
{
    bus_.DropAll();
    for (uint32_t port = 0; port <= MaxRootPorts; ++port)
    {
        port_slot_[port] = 0;
        port_failed_[port] = false;
    }
}

void Manager::Service()
{
    host_.ServiceEndpoints();
    for (uint32_t slot = 1; slot <= MaxDeviceSlots; ++slot)
    {
        if (!unbind_[slot].exchange(false))
        {
            continue;
        }
        Device* device = bus_.Find(slot);
        if (device != nullptr)
        {
            Log(device->path, "too many transfer errors, drivers unbound", true);
            bus_.UnbindDrivers(*device);
        }
    }
}

void Manager::Run()
{
    if (host_.Recover())
    {
        Forget();
    }
    Service();
    for (;;)
    {
        uint32_t port;
        DeviceWork work;
        if (host_.TakePortChange(&port))
        {
            HandlePort(port);
        }
        else if (bus_.TakeWork(&work))
        {
            HandleHub(work);
        }
        else
        {
            return;
        }
        Service();
    }
}

void Manager::HandlePort(uint32_t port)
{
    if (port == 0 || port > MaxRootPorts)
    {
        return;
    }
    RootPortStatus status;
    host_.ReadRootPort(port, &status, true);
    const uint32_t slot = port_slot_[port];
    if (status.connect_changed)
    {
        port_failed_[port] = false;
    }
    if (!status.connected || (status.connect_changed && slot != 0))
    {
        if (slot != 0)
        {
            Device* device = bus_.Find(slot);
            if (device != nullptr)
            {
                bus_.Detach(*device);
            }
            port_slot_[port] = 0;
        }
        if (!status.connected)
        {
            return;
        }
    }
    else if (slot != 0 || port_failed_[port])
    {
        return;
    }
    host_.Delay(DebounceMs);
    host_.ReadRootPort(port, &status, false);
    if (!status.connected)
    {
        return;
    }
    Speed speed;
    char path[MaxPathName];
    MakePath(path, sizeof(path), bus_.Number(), nullptr, port);
    if (!host_.ResetRootPort(port, &speed))
    {
        Log(path, "port did not reset", true);
        port_failed_[port] = true;
        return;
    }
    const Location location = {speed, static_cast<uint8_t>(port), 0, 0, 0, 0, false};
    Device* device = Enumerate(location, nullptr, static_cast<uint8_t>(port));
    port_failed_[port] = device == nullptr;
    if (device != nullptr)
    {
        port_slot_[port] = device->slot;
    }
}

Status Manager::GetDescriptor(uint32_t slot, uint8_t type, uint8_t index, uint16_t language,
                              uint16_t length, Memory::Dma::Buffer& buffer, uint32_t* actual)
{
    const SetupPacket setup = {DeviceToHostStandard, GetDescriptorRequest,
                               static_cast<uint16_t>((type << 8) | index), language, length};
    return host_.Control(slot, setup, buffer, actual);
}

Device* Manager::Enumerate(const Location& location, Device* parent, uint8_t port)
{
    char path[MaxPathName];
    MakePath(path, sizeof(path), bus_.Number(), parent, port);
    uint32_t slot = 0;
    switch (host_.CreateDevice(location, &slot))
    {
    case CreateStatus::Created:
        break;
    case CreateStatus::NoSlots:
        Log(path, "no device slots left", true);
        return nullptr;
    case CreateStatus::SlotFailed:
        Log(path, "could not set up its device slot", true);
        return nullptr;
    case CreateStatus::AddressFailed:
        Log(path, "could not be addressed", true);
        return nullptr;
    case CreateStatus::Unavailable:
        Log(path, "got no device slot", true);
        return nullptr;
    }
    Device& info = bus_.Prepare(slot, location);
    Memory::Dma::Buffer buffer{};
    Step failed = Step::Gone;
    const bool read = host_.AllocateBuffer(MaxConfigurationBytes, &buffer) &&
                      ReadDescriptors(info, buffer, &failed);
    if (buffer.size != 0)
    {
        host_.FreeBuffer(buffer);
    }
    if (!read)
    {
        host_.DestroyDevice(slot);
        switch (failed)
        {
        case Step::Gone:
            Log(path, "the device went away during enumeration", true);
            break;
        case Step::ConfigurationSize:
            Log(path, "configuration descriptor over 4096 bytes, refused", true);
            break;
        case Step::ControlPacket:
            Log(path, "invalid control endpoint packet size", true);
            break;
        default:
            Log(path, "enumeration failed", true);
            break;
        }
        return nullptr;
    }
    bus_.Attach(info, parent, parent == nullptr ? 0 : port);
    return &info;
}

bool Manager::ReadDescriptors(Device& info, Memory::Dma::Buffer& buffer, Step* failed)
{
    const uint32_t slot = info.slot;
    const Speed speed = info.location.speed;
    uint32_t actual = 0;
    const auto fail = [&](Status status, Step step)
    {
        *failed = status == Status::NoDevice ? Step::Gone : step;
        return false;
    };
    const auto copy = [&](uint8_t* destination, uint32_t length)
    {
        for (uint32_t index = 0; index < length; ++index)
        {
            destination[index] = buffer.address[index];
        }
    };

    Status status =
        GetDescriptor(slot, DescriptorType::Device, 0, 0, ShortDeviceBytes, buffer, &actual);
    if (!Succeeded(status) || actual < ShortDeviceBytes)
    {
        return fail(status, Step::ShortDevice);
    }
    const uint32_t packet = PacketFromDescriptor(speed, buffer.address[7]);
    if (!ValidControlPacket(speed, packet))
    {
        return fail(Status::Success, Step::ControlPacket);
    }
    if (packet != DefaultControlPacket(speed) && !host_.SetControlMaxPacket(slot, packet))
    {
        return fail(Status::Success, Step::ControlPacket);
    }

    uint8_t bytes[DeviceDescriptorBytes];
    status =
        GetDescriptor(slot, DescriptorType::Device, 0, 0, DeviceDescriptorBytes, buffer, &actual);
    if (!Succeeded(status) || actual < DeviceDescriptorBytes)
    {
        return fail(status, Step::Device);
    }
    copy(bytes, DeviceDescriptorBytes);
    if (ParseDevice(bytes, &info.device) != ParseStatus::Ok)
    {
        return fail(Status::Success, Step::Device);
    }

    status = GetDescriptor(slot, DescriptorType::Configuration, 0, 0, ConfigurationHeaderBytes,
                           buffer, &actual);
    if (!Succeeded(status) || actual < ConfigurationHeaderBytes)
    {
        return fail(status, Step::ConfigurationHeader);
    }
    const uint32_t total = buffer.address[2] | (buffer.address[3] << 8);
    if (total > MaxConfigurationBytes)
    {
        return fail(Status::Success, Step::ConfigurationSize);
    }
    if (total < ConfigurationHeaderBytes)
    {
        return fail(Status::Success, Step::ConfigurationHeader);
    }
    status = GetDescriptor(slot, DescriptorType::Configuration, 0, 0, static_cast<uint16_t>(total),
                           buffer, &actual);
    if (!Succeeded(status) || actual < total)
    {
        return fail(status, Step::Configuration);
    }
    copy(configuration_bytes_, total);
    if (ParseConfiguration(std::span<const uint8_t>(configuration_bytes_, total),
                           &info.configuration) != ParseStatus::Ok)
    {
        return fail(Status::Success, Step::ConfigurationParse);
    }

    info.product[0] = '\0';
    if (info.device.product_index != 0)
    {
        status = GetDescriptor(slot, DescriptorType::String, 0, 0, LanguageBytes, buffer, &actual);
        if (status == Status::NoDevice)
        {
            return fail(status, Step::Gone);
        }
        if (Succeeded(status) && actual >= LanguageBytes)
        {
            const uint16_t language =
                static_cast<uint16_t>(buffer.address[2] | (buffer.address[3] << 8));
            status = GetDescriptor(slot, DescriptorType::String, info.device.product_index,
                                   language, StringBytes, buffer, &actual);
            if (status == Status::NoDevice)
            {
                return fail(status, Step::Gone);
            }
            if (Succeeded(status) && actual >= 2)
            {
                uint8_t text[StringBytes];
                copy(text, actual < StringBytes ? actual : StringBytes);
                DecodeString(
                    std::span<const uint8_t>(text, actual < StringBytes ? actual : StringBytes),
                    info.product, MaxProductName);
            }
        }
    }
    return true;
}

bool Manager::ReadPortStatus(Hub& hub, uint32_t port, HubPort::Event* event)
{
    uint16_t status = 0;
    uint16_t change = 0;
    if (hub.GetPortStatus(port, &status, &change) != Status::Success)
    {
        return false;
    }
    *event = {HubPort::EventKind::Status, status, change};
    return true;
}

void Manager::ServiceHubStatus(Hub& hub)
{
    uint16_t status = 0;
    uint16_t change = 0;
    if (hub.GetHubStatus(&status, &change) != Status::Success)
    {
        return;
    }
    if ((change & 1U) != 0)
    {
        hub.ClearHubFeature(ClearLocalPower);
    }
    if ((change & HubStatusChangeBits & ~1U) != 0)
    {
        hub.ClearHubFeature(ClearHubOverCurrent);
    }
}

void Manager::HandleHub(const DeviceWork& work)
{
    Device* device = bus_.FindNumber(work.number);
    if (device == nullptr || device->detaching.load() ||
        device->generation.load() != work.generation)
    {
        return;
    }
    Hub* hub = HubOf(*device);
    if (hub == nullptr)
    {
        return;
    }
    const uint32_t bits = hub->TakePending();
    if ((bits & 1U) != 0)
    {
        ServiceHubStatus(*hub);
    }
    for (uint32_t port = 1; port <= hub->ports; ++port)
    {
        if ((bits & (1U << port)) != 0 && !device->detaching.load())
        {
            ServiceHubPort(*device, *hub, port);
        }
    }
    if (!device->detaching.load())
    {
        hub->Listen();
    }
}

void Manager::ServiceHubPort(Device& hub_device, Hub& hub, uint32_t port)
{
    HubPort::State& state = hub.port[port];
    HubPort::Event event = {HubPort::EventKind::Status};
    char path[MaxPathName];
    MakePath(path, sizeof(path), bus_.Number(), &hub_device, port);
    if (state.phase == HubPort::Phase::PowerOn)
    {
        event.kind = HubPort::EventKind::PoweredUp;
    }
    else if (!ReadPortStatus(hub, port, &event))
    {
        return;
    }
    uint64_t reset_deadline = 0;
    for (uint32_t round = 0; round < MaxPortRounds; ++round)
    {
        const HubPort::Phase before = state.phase;
        const HubPort::EventKind kind = event.kind;
        const HubPort::Action action = HubPort::Step(state, event);
        for (uint16_t bit = 1; bit != 0 && bit <= HubPort::ChangeBit::ConfigError; bit <<= 1)
        {
            const uint8_t selector = HubPort::ClearSelector(state.usb3, bit);
            if ((action.clear & bit) != 0 && selector != 0 &&
                hub.ClearPortFeature(port, selector) != Status::Success)
            {
                return;
            }
        }
        if (action.detach)
        {
            Device* child = bus_.ChildAt(hub_device, static_cast<uint8_t>(port));
            if (child != nullptr)
            {
                bus_.Detach(*child);
            }
        }
        if (action.power_off)
        {
            hub.ClearPortFeature(port, HubPort::Feature::PortPower);
            Log(path, "over-current, port powered off", true);
        }
        if (kind == HubPort::EventKind::ResetTimeout)
        {
            Log(path, "port did not reset", true);
        }
        else if (before == HubPort::Phase::Debounce && state.phase == HubPort::Phase::Failed)
        {
            Log(path, "connection did not settle", true);
        }
        else if (before == HubPort::Phase::Reset && state.phase == HubPort::Phase::Failed)
        {
            Log(path, "port did not enable after its reset", true);
        }
        switch (action.next)
        {
        case HubPort::Next::None:
            return;
        case HubPort::Next::ReadStatus:
            if (!ReadPortStatus(hub, port, &event))
            {
                return;
            }
            break;
        case HubPort::Next::Debounce:
            host_.Delay(DebounceMs);
            if (!ReadPortStatus(hub, port, &event))
            {
                return;
            }
            break;
        case HubPort::Next::Reset:
            if (hub.SetPortFeature(port, HubPort::Feature::PortReset) != Status::Success)
            {
                event = {HubPort::EventKind::ResetTimeout};
                break;
            }
            reset_deadline = host_.NowMilliseconds() + HubPort::ResetTimeoutMilliseconds;
            [[fallthrough]];
        case HubPort::Next::WaitReset:
            if (host_.NowMilliseconds() >= reset_deadline)
            {
                event = {HubPort::EventKind::ResetTimeout};
                break;
            }
            host_.Delay(ResetPollMs);
            if (!ReadPortStatus(hub, port, &event))
            {
                return;
            }
            break;
        case HubPort::Next::Enumerate:
        {
            Location location;
            const HubPort::HubSite site = {hub_device.location, hub_device.slot, hub.multi_tt};
            if (!HubPort::ChildLocation(site, static_cast<uint8_t>(port), action.speed, &location))
            {
                Log(path, "too many hub tiers, port refused", true);
                hub.ClearPortFeature(port, HubPort::Feature::PortEnable);
                event = {HubPort::EventKind::EnumerationFailed};
                break;
            }
            Device* child = Enumerate(location, &hub_device, static_cast<uint8_t>(port));
            event = {child != nullptr ? HubPort::EventKind::Enumerated
                                      : HubPort::EventKind::EnumerationFailed};
            break;
        }
        }
    }
}

} // namespace FeatherOS::Usb
