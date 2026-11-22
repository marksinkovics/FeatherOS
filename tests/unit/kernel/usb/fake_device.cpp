#include "fake_device.h"

#include <algorithm>
#include <memory>

namespace FeatherOS::Usb::Testing
{
namespace
{

constexpr uint8_t GetDescriptor = 6;
constexpr uint8_t SetConfiguration = 9;

void Append(std::vector<uint8_t>& bytes, std::initializer_list<uint8_t> values)
{
    bytes.insert(bytes.end(), values.begin(), values.end());
}

void Interface(std::vector<uint8_t>& bytes, uint8_t number, uint8_t alternate, uint8_t endpoints,
               uint8_t class_code, uint8_t subclass, uint8_t protocol)
{
    Append(bytes, {9, 4, number, alternate, endpoints, class_code, subclass, protocol, 0});
}

void Endpoint(std::vector<uint8_t>& bytes, uint8_t address, uint8_t attributes, uint16_t packet,
              uint8_t interval)
{
    Append(bytes, {7, 5, address, attributes, static_cast<uint8_t>(packet),
                   static_cast<uint8_t>(packet >> 8), interval});
}

void Hid(std::vector<uint8_t>& bytes, uint16_t report_length)
{
    Append(bytes, {9, 0x21, 0x11, 0x01, 0, 1, 0x22, static_cast<uint8_t>(report_length),
                   static_cast<uint8_t>(report_length >> 8)});
}

std::vector<uint8_t> Finish(std::vector<uint8_t> body, uint8_t interfaces)
{
    std::vector<uint8_t> bytes;
    const auto total = static_cast<uint16_t>(9 + body.size());
    Append(bytes, {9, 2, static_cast<uint8_t>(total), static_cast<uint8_t>(total >> 8), interfaces,
                   1, 0, 0xA0, 50});
    bytes.insert(bytes.end(), body.begin(), body.end());
    return bytes;
}

} // namespace

ScriptedDevice::ScriptedDevice(std::vector<uint8_t> device, std::vector<uint8_t> configuration)
    : device_(std::move(device)), configuration_(std::move(configuration))
{
    AddString(1, "FeatherOS test");
    AddString(2, "Scripted Device");
}

void ScriptedDevice::AddString(uint8_t index, const std::string& text)
{
    std::vector<uint8_t> descriptor = {static_cast<uint8_t>(2 + text.size() * 2), 3};
    for (const char character : text)
    {
        descriptor.push_back(static_cast<uint8_t>(character));
        descriptor.push_back(0);
    }
    strings_[index] = descriptor;
}

uint32_t ScriptedDevice::MaxPacket0(Speed speed) const
{
    const uint8_t value = device_[7];
    return speed >= Speed::Super ? 1U << value : value;
}

void ScriptedDevice::FailControl(Outcome outcome, int count, int request)
{
    control_faults_.push_back({outcome, count, request});
}

ScriptedDevice::Outcome ScriptedDevice::Control(const Request& request,
                                                std::vector<uint8_t>* response)
{
    requests_.push_back(request);
    response->clear();
    if (on_request)
    {
        on_request(request);
    }
    for (Fault& fault : control_faults_)
    {
        if (fault.remaining > 0 && (fault.request < 0 || fault.request == request.request))
        {
            --fault.remaining;
            return fault.outcome;
        }
    }
    if (request.type == 0x80 && request.request == GetDescriptor)
    {
        const auto type = static_cast<uint8_t>(request.value >> 8);
        const auto index = static_cast<uint8_t>(request.value);
        const std::vector<uint8_t>* source = nullptr;
        std::vector<uint8_t> languages = {4, 3, 0x09, 0x04};
        if (type == 1)
        {
            source = &device_;
        }
        else if (type == 2)
        {
            source = &configuration_;
        }
        else if (type == 3 && index == 0)
        {
            source = &languages;
        }
        else if (type == 3 && strings_.count(index) != 0)
        {
            source = &strings_[index];
        }
        if (source == nullptr)
        {
            return Outcome::Stall;
        }
        const size_t count = std::min<size_t>(request.length, source->size());
        response->assign(source->begin(), source->begin() + static_cast<long>(count));
        return Outcome::Ok;
    }
    if (request.type == 0x00 && request.request == SetConfiguration)
    {
        configuration_value_ = static_cast<uint8_t>(request.value);
    }
    return Outcome::Ok;
}

void ScriptedDevice::QueueReport(uint8_t endpoint, std::vector<uint8_t> report)
{
    reports_[endpoint].push_back(std::move(report));
}

bool ScriptedDevice::HasReport(uint8_t endpoint) const
{
    const auto found = reports_.find(endpoint);
    return found != reports_.end() && !found->second.empty();
}

bool ScriptedDevice::TakeReport(uint8_t endpoint, std::vector<uint8_t>* report)
{
    if (!HasReport(endpoint))
    {
        return false;
    }
    *report = reports_[endpoint].front();
    reports_[endpoint].pop_front();
    return true;
}

void ScriptedDevice::FailEndpoint(uint8_t endpoint, Outcome outcome, int count)
{
    endpoint_faults_[endpoint].push_back({outcome, count, -1});
}

ScriptedDevice::Outcome ScriptedDevice::EndpointOutcome(uint8_t endpoint)
{
    for (Fault& fault : endpoint_faults_[endpoint])
    {
        if (fault.remaining > 0)
        {
            --fault.remaining;
            return fault.outcome;
        }
    }
    return Outcome::Ok;
}

const std::vector<ScriptedDevice::Request>& ScriptedDevice::Requests() const
{
    return requests_;
}

uint32_t ScriptedDevice::Count(uint8_t type, uint8_t request) const
{
    uint32_t count = 0;
    for (const Request& entry : requests_)
    {
        count += entry.type == type && entry.request == request ? 1 : 0;
    }
    return count;
}

uint8_t ScriptedDevice::configuration() const
{
    return configuration_value_;
}

std::vector<uint8_t> MakeDeviceDescriptor(uint16_t vendor, uint16_t product, uint8_t max_packet0,
                                           uint8_t device_class, uint8_t product_string,
                                           uint16_t usb_version)
{
    return {18,
            1,
            static_cast<uint8_t>(usb_version),
            static_cast<uint8_t>(usb_version >> 8),
            device_class,
            0,
            0,
            max_packet0,
            static_cast<uint8_t>(vendor),
            static_cast<uint8_t>(vendor >> 8),
            static_cast<uint8_t>(product),
            static_cast<uint8_t>(product >> 8),
            0x00,
            0x01,
            1,
            product_string,
            0,
            1};
}

std::vector<uint8_t> KeyboardConfiguration()
{
    std::vector<uint8_t> body;
    Interface(body, 0, 0, 1, 3, 1, 1);
    Hid(body, 63);
    Endpoint(body, 0x81, 3, 8, 10);
    return Finish(body, 1);
}

std::vector<uint8_t> MouseConfiguration()
{
    std::vector<uint8_t> body;
    Interface(body, 0, 0, 1, 3, 1, 2);
    Hid(body, 52);
    Endpoint(body, 0x81, 3, 4, 10);
    return Finish(body, 1);
}

std::vector<uint8_t> ComboConfiguration()
{
    std::vector<uint8_t> body;
    Interface(body, 0, 0, 1, 3, 1, 1);
    Hid(body, 63);
    Endpoint(body, 0x81, 3, 8, 10);
    Interface(body, 1, 0, 1, 3, 1, 2);
    Hid(body, 52);
    Endpoint(body, 0x82, 3, 4, 10);
    return Finish(body, 2);
}

std::vector<uint8_t> HubConfiguration()
{
    std::vector<uint8_t> body;
    Interface(body, 0, 0, 1, 9, 0, 1);
    Endpoint(body, 0x81, 3, 1, 12);
    Interface(body, 0, 1, 1, 9, 0, 2);
    Endpoint(body, 0x81, 3, 1, 12);
    return Finish(body, 1);
}

std::vector<uint8_t> MassStorageConfiguration()
{
    std::vector<uint8_t> body;
    Interface(body, 0, 0, 2, 8, 6, 0x50);
    Endpoint(body, 0x81, 2, 512, 0);
    Append(body, {6, 0x30, 15, 0, 0, 0});
    Endpoint(body, 0x02, 2, 512, 0);
    Append(body, {6, 0x30, 15, 0, 0, 0});
    return Finish(body, 1);
}

uint8_t MaxPacket0Field(Speed speed)
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
    return 9;
}

std::shared_ptr<ScriptedDevice> MakeKeyboard(Speed speed, uint8_t max_packet0)
{
    auto device = std::make_shared<ScriptedDevice>(
        MakeDeviceDescriptor(0x0627, 0x0001, max_packet0 != 0 ? max_packet0 : MaxPacket0Field(speed)),
        KeyboardConfiguration());
    device->AddString(2, "QEMU USB Keyboard");
    return device;
}

std::shared_ptr<ScriptedDevice> MakeMouse(Speed speed, uint8_t max_packet0)
{
    auto device = std::make_shared<ScriptedDevice>(
        MakeDeviceDescriptor(0x0627, 0x0002, max_packet0 != 0 ? max_packet0 : MaxPacket0Field(speed)),
        MouseConfiguration());
    device->AddString(2, "QEMU USB Mouse");
    return device;
}

} // namespace FeatherOS::Usb::Testing
