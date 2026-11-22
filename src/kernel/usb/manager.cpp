#include "manager.h"

#include "kernel/klogger.h"
#include "kernel/usb/text.h"

namespace FeatherOS::Usb
{
namespace
{

constexpr uint32_t DebounceMs = 100;
constexpr uint8_t DeviceToHostStandard = 0x80;
constexpr uint8_t GetDescriptorRequest = 6;
constexpr uint32_t ShortDeviceBytes = 8;
constexpr uint32_t LanguageBytes = 4;
constexpr uint32_t StringBytes = 128;

constinit KLogger logger;

const char* SpeedName(Speed speed)
{
    switch (speed)
    {
    case Speed::Low:
        return "low-speed";
    case Speed::Full:
        return "full-speed";
    case Speed::High:
        return "high-speed";
    case Speed::Super:
        return "super-speed";
    case Speed::SuperPlus:
        return "super-speed+";
    }
    return "unknown-speed";
}

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

void Log(uint32_t port, const char* message, bool warning)
{
    char line[96];
    Text text(line, sizeof(line));
    text.Words("USB 1-");
    text.Decimal(port);
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

Manager::Manager(HostController& host) : host_(host) {}

bool Manager::HasWork() const
{
    return host_.HasWork();
}

const DeviceInfo* Manager::Find(uint32_t slot) const
{
    return slot != 0 && slot <= MaxDeviceSlots && known_[slot] ? &devices_[slot] : nullptr;
}

uint32_t Manager::SlotOfPort(uint32_t port) const
{
    return port <= MaxRootPorts ? port_slot_[port] : 0;
}

void Manager::Forget()
{
    for (uint32_t port = 0; port <= MaxRootPorts; ++port)
    {
        port_slot_[port] = 0;
        port_failed_[port] = false;
    }
    for (uint32_t slot = 0; slot <= MaxDeviceSlots; ++slot)
    {
        known_[slot] = false;
    }
}

void Manager::Run()
{
    if (host_.Recover())
    {
        Forget();
    }
    host_.ServiceEndpoints();
    uint32_t port;
    while (host_.TakePortChange(&port))
    {
        HandlePort(port);
        host_.ServiceEndpoints();
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
            host_.DestroyDevice(slot);
            known_[slot] = false;
            port_slot_[port] = 0;
            Log(port, "disconnected", false);
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
    port_failed_[port] = !Enumerate(port);
}

Status Manager::GetDescriptor(uint32_t slot, uint8_t type, uint8_t index, uint16_t language,
                              uint16_t length, Memory::Dma::Buffer& buffer, uint32_t* actual)
{
    const SetupPacket setup = {DeviceToHostStandard, GetDescriptorRequest,
                               static_cast<uint16_t>((type << 8) | index), language, length};
    return host_.Control(slot, setup, buffer, actual);
}

bool Manager::Enumerate(uint32_t port)
{
    Speed speed;
    if (!host_.ResetRootPort(port, &speed))
    {
        Log(port, "port did not reset", true);
        return false;
    }
    const Location location = {speed, static_cast<uint8_t>(port), 0, 0, 0, 0, false};
    uint32_t slot = 0;
    switch (host_.CreateDevice(location, &slot))
    {
    case CreateStatus::Created:
        break;
    case CreateStatus::NoSlots:
        Log(port, "no device slots left", true);
        return false;
    case CreateStatus::SlotFailed:
        Log(port, "could not set up its device slot", true);
        return false;
    case CreateStatus::AddressFailed:
        Log(port, "could not be addressed", true);
        return false;
    case CreateStatus::Unavailable:
        Log(port, "got no device slot", true);
        return false;
    }
    DeviceInfo& info = devices_[slot];
    info = {};
    info.slot = slot;
    info.location = location;
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
            Log(port, "the device went away during enumeration", true);
            break;
        case Step::ConfigurationSize:
            Log(port, "configuration descriptor over 4096 bytes, refused", true);
            break;
        case Step::ControlPacket:
            Log(port, "invalid control endpoint packet size", true);
            break;
        default:
            Log(port, "enumeration failed", true);
            break;
        }
        return false;
    }
    known_[slot] = true;
    port_slot_[port] = slot;
    char line[128];
    Text text(line, sizeof(line));
    text.Words("USB 1-");
    text.Decimal(port);
    text.Words(": ");
    text.Words(SpeedName(speed));
    text.Put(' ');
    text.Hex(info.device.vendor, 4);
    text.Put(':');
    text.Hex(info.device.product, 4);
    if (info.product[0] != '\0')
    {
        text.Words(" \"");
        text.Words(info.product);
        text.Put('"');
    }
    if (text.Finish())
    {
        logger.Info(line);
    }
    return true;
}

bool Manager::ReadDescriptors(DeviceInfo& info, Memory::Dma::Buffer& buffer, Step* failed)
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

} // namespace FeatherOS::Usb
