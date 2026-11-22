#include "fake_device.h"

#include <algorithm>
#include <memory>

namespace FeatherOS::Usb::Testing
{
namespace
{

constexpr uint8_t GetDescriptor = 6;
constexpr uint8_t SetConfiguration = 9;
constexpr uint8_t ClearFeature = 1;
constexpr uint16_t EndpointHalt = 0;

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

void ScriptedDevice::SetReportDescriptor(uint8_t interface, std::vector<uint8_t> descriptor)
{
    report_descriptors_[interface] = std::move(descriptor);
}

void ScriptedDevice::RecordData(const Request& request, std::vector<uint8_t> data)
{
    data_.emplace_back(request, std::move(data));
}

std::vector<std::vector<uint8_t>> ScriptedDevice::DataOf(uint8_t request) const
{
    std::vector<std::vector<uint8_t>> found;
    for (const auto& [entry, data] : data_)
    {
        if (entry.request == request)
        {
            found.push_back(data);
        }
    }
    return found;
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
    Outcome handled = Outcome::Ok;
    if (HandleClass(request, response, &handled))
    {
        return handled;
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
    if (request.type == 0x81 && request.request == GetDescriptor && (request.value >> 8) == 0x22)
    {
        const auto found = report_descriptors_.find(static_cast<uint8_t>(request.index));
        if (found == report_descriptors_.end())
        {
            return Outcome::Stall;
        }
        const size_t count = std::min<size_t>(request.length, found->second.size());
        response->assign(found->second.begin(), found->second.begin() + static_cast<long>(count));
        return Outcome::Ok;
    }
    if (request.type == 0x00 && request.request == SetConfiguration)
    {
        configuration_value_ = static_cast<uint8_t>(request.value);
    }
    if (request.type == 0x02 && request.request == ClearFeature && request.value == EndpointHalt)
    {
        toggles_[static_cast<uint8_t>(request.index)] = false;
    }
    return Outcome::Ok;
}

bool ScriptedDevice::HandleClass(const Request&, std::vector<uint8_t>*, Outcome*)
{
    return false;
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

void ScriptedDevice::ReceiveBulk(uint8_t endpoint, std::span<const uint8_t> data)
{
    bulk_out_[endpoint].emplace_back(data.begin(), data.end());
}

ScriptedDevice::Outcome ScriptedDevice::ProduceBulk(uint8_t endpoint, uint32_t max,
                                                    std::vector<uint8_t>* out)
{
    out->clear();
    const Outcome outcome = EndpointOutcome(endpoint);
    if (outcome != Outcome::Ok)
    {
        return outcome;
    }
    std::vector<uint8_t> report;
    if (!TakeReport(endpoint, &report))
    {
        return Outcome::Hang;
    }
    if (report.size() > max)
    {
        reports_[endpoint].push_front(
            std::vector<uint8_t>(report.begin() + static_cast<long>(max), report.end()));
        report.resize(max);
    }
    *out = std::move(report);
    return Outcome::Ok;
}

const std::vector<std::vector<uint8_t>>& ScriptedDevice::BulkReceived(uint8_t endpoint) const
{
    static const std::vector<std::vector<uint8_t>> none;
    const auto found = bulk_out_.find(endpoint);
    return found == bulk_out_.end() ? none : found->second;
}

bool ScriptedDevice::Toggle(uint8_t endpoint) const
{
    const auto found = toggles_.find(endpoint);
    return found != toggles_.end() && found->second;
}

void ScriptedDevice::FlipToggle(uint8_t endpoint)
{
    toggles_[endpoint] = !Toggle(endpoint);
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

std::vector<uint8_t> GenericKeyboardConfiguration(uint16_t report_length)
{
    std::vector<uint8_t> body;
    Interface(body, 0, 0, 1, 3, 0, 0);
    Hid(body, report_length);
    Endpoint(body, 0x81, 3, 8, 10);
    return Finish(body, 1);
}

std::vector<uint8_t> GenericMouseConfiguration(uint16_t report_length, bool boot_interface)
{
    std::vector<uint8_t> body;
    Interface(body, 0, 0, 1, 3, boot_interface ? 1 : 0, boot_interface ? 2 : 0);
    Hid(body, report_length);
    Endpoint(body, 0x81, 3, 8, 10);
    return Finish(body, 1);
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

std::vector<uint8_t> SuperSpeedHubConfiguration()
{
    std::vector<uint8_t> body;
    Interface(body, 0, 0, 1, 9, 0, 0);
    Endpoint(body, 0x81, 3, 2, 12);
    Append(body, {6, 0x30, 0, 0, 2, 0});
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

const std::vector<uint8_t>& BootKeyboardDescriptor()
{
    static const std::vector<uint8_t> descriptor = {
        0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7, 0x15, 0x00,
        0x25, 0x01, 0x75, 0x01, 0x95, 0x08, 0x81, 0x02, 0x95, 0x01, 0x75, 0x08, 0x81, 0x01,
        0x95, 0x05, 0x75, 0x01, 0x05, 0x08, 0x19, 0x01, 0x29, 0x05, 0x91, 0x02, 0x95, 0x01,
        0x75, 0x03, 0x91, 0x01, 0x95, 0x06, 0x75, 0x08, 0x15, 0x00, 0x25, 0x65, 0x05, 0x07,
        0x19, 0x00, 0x29, 0x65, 0x81, 0x00, 0xC0};
    return descriptor;
}

const std::vector<uint8_t>& BootMouseDescriptor()
{
    static const std::vector<uint8_t> descriptor = {
        0x05, 0x01, 0x09, 0x02, 0xA1, 0x01, 0x09, 0x01, 0xA1, 0x00, 0x05, 0x09, 0x19, 0x01,
        0x29, 0x03, 0x15, 0x00, 0x25, 0x01, 0x95, 0x03, 0x75, 0x01, 0x81, 0x02, 0x95, 0x01,
        0x75, 0x05, 0x81, 0x01, 0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x15, 0x81, 0x25, 0x7F,
        0x75, 0x08, 0x95, 0x02, 0x81, 0x06, 0xC0, 0xC0};
    return descriptor;
}

ScriptedHub::ScriptedHub(bool super_speed, uint8_t ports, std::vector<uint8_t> device,
                         std::vector<uint8_t> configuration, uint8_t think_time)
    : ScriptedDevice(std::move(device), std::move(configuration)),
      super_speed_(super_speed),
      think_time_(think_time),
      ports_(static_cast<size_t>(ports) + 1)
{
}

uint16_t ScriptedHub::SpeedBits(Speed speed) const
{
    if (super_speed_)
    {
        return 0;
    }
    return speed == Speed::Low ? 0x0200 : (speed == Speed::High ? 0x0400 : 0);
}

void ScriptedHub::Plug(uint8_t port, std::shared_ptr<ScriptedDevice> child, Speed speed)
{
    Port& entry = ports_[port];
    entry.child = std::move(child);
    entry.speed = speed;
    entry.status = static_cast<uint16_t>((entry.status & ~(0x0600 | StatusEnable)) |
                                         StatusConnection | SpeedBits(speed));
    entry.change |= 0x0001;
}

void ScriptedHub::Unplug(uint8_t port)
{
    Port& entry = ports_[port];
    entry.child.reset();
    entry.status = static_cast<uint16_t>(entry.status &
                                         ~(StatusConnection | StatusEnable | 0x0600));
    entry.change |= 0x0001;
}

void ScriptedHub::OverCurrent(uint8_t port)
{
    Port& entry = ports_[port];
    entry.status |= StatusOverCurrent;
    entry.change |= 0x0008;
}

void ScriptedHub::Flap(uint8_t port)
{
    ports_[port].change |= 0x0001;
}

void ScriptedHub::HoldReset(uint8_t port, bool hold)
{
    ports_[port].hold_reset = hold;
}

void ScriptedHub::SkipResetChange(uint8_t port, bool skip)
{
    ports_[port].skip_reset_change = skip;
}

std::shared_ptr<ScriptedDevice> ScriptedHub::Child(uint8_t port) const
{
    return port < ports_.size() ? ports_[port].child : nullptr;
}

uint8_t ScriptedHub::Ports() const
{
    return static_cast<uint8_t>(ports_.size() - 1);
}

bool ScriptedHub::SuperSpeed() const
{
    return super_speed_;
}

uint16_t ScriptedHub::Status(uint8_t port) const
{
    return ports_[port].status;
}

uint16_t ScriptedHub::Change(uint8_t port) const
{
    return ports_[port].change;
}

bool ScriptedHub::Powered(uint8_t port) const
{
    return ports_[port].powered;
}

uint32_t ScriptedHub::Resets(uint8_t port) const
{
    return ports_[port].resets;
}

const std::vector<std::pair<uint8_t, uint8_t>>& ScriptedHub::PortSets() const
{
    return sets_;
}

const std::vector<std::pair<uint8_t, uint8_t>>& ScriptedHub::PortClears() const
{
    return clears_;
}

const std::vector<uint16_t>& ScriptedHub::Depths() const
{
    return depths_;
}

const std::vector<uint16_t>& ScriptedHub::Alternates() const
{
    return alternates_;
}

const std::vector<uint8_t>& ScriptedHub::ResetOrder() const
{
    return reset_order_;
}

std::vector<uint8_t> ScriptedHub::Descriptor() const
{
    const uint8_t ports = Ports();
    const uint8_t characteristics = static_cast<uint8_t>(think_time_ << 5);
    std::vector<uint8_t> bytes;
    if (super_speed_)
    {
        Append(bytes, {12, 0x2A, ports, characteristics, 0, 5, 0, 0, 0, 0, 0, 0});
        return bytes;
    }
    const uint8_t removable = static_cast<uint8_t>((ports + 8) / 8);
    Append(bytes, {static_cast<uint8_t>(7 + 2 * removable), 0x29, ports, characteristics, 0, 5, 0});
    bytes.insert(bytes.end(), static_cast<size_t>(removable) * 2, 0);
    return bytes;
}

bool ScriptedHub::HasReport(uint8_t endpoint) const
{
    if (ScriptedDevice::HasReport(endpoint))
    {
        return true;
    }
    for (const Port& port : ports_)
    {
        if (port.change != 0)
        {
            return endpoint == 0x81;
        }
    }
    return false;
}

bool ScriptedHub::TakeReport(uint8_t endpoint, std::vector<uint8_t>* report)
{
    if (ScriptedDevice::HasReport(endpoint))
    {
        return ScriptedDevice::TakeReport(endpoint, report);
    }
    if (endpoint != 0x81)
    {
        return false;
    }
    uint32_t bits = 0;
    for (size_t port = 1; port < ports_.size(); ++port)
    {
        bits |= ports_[port].change != 0 ? 1U << port : 0;
    }
    if (bits == 0)
    {
        return false;
    }
    report->assign({static_cast<uint8_t>(bits), static_cast<uint8_t>(bits >> 8)});
    if (ports_.size() <= 8)
    {
        report->resize(1);
    }
    return true;
}

bool ScriptedHub::HandleClass(const Request& request, std::vector<uint8_t>* response,
                              Outcome* outcome)
{
    const auto port_index = static_cast<uint8_t>(request.index);
    if (request.type == 0xA0 && request.request == GetDescriptor)
    {
        const std::vector<uint8_t> descriptor = Descriptor();
        const auto type = static_cast<uint8_t>(request.value >> 8);
        if (type != descriptor[1])
        {
            *outcome = Outcome::Stall;
            return true;
        }
        const size_t count = std::min<size_t>(request.length, descriptor.size());
        response->assign(descriptor.begin(), descriptor.begin() + static_cast<long>(count));
        return true;
    }
    if (request.type == 0xA0 && request.request == 0)
    {
        response->assign({0, 0, 0, 0});
        return true;
    }
    if (request.type == 0x20 && request.request == 12)
    {
        depths_.push_back(request.value);
        return true;
    }
    if (request.type == 0x01 && request.request == 11)
    {
        alternates_.push_back(request.value);
        return true;
    }
    if (request.type != 0x23 && request.type != 0xA3)
    {
        return false;
    }
    if (port_index == 0 || port_index >= ports_.size())
    {
        *outcome = Outcome::Stall;
        return true;
    }
    Port& port = ports_[port_index];
    const auto feature = static_cast<uint8_t>(request.value);
    if (request.type == 0xA3 && request.request == 0)
    {
        response->assign({static_cast<uint8_t>(port.status), static_cast<uint8_t>(port.status >> 8),
                          static_cast<uint8_t>(port.change), static_cast<uint8_t>(port.change >> 8)});
        return true;
    }
    if (request.request == 3)
    {
        sets_.emplace_back(port_index, feature);
        if (feature == 8)
        {
            port.powered = true;
            port.status |= super_speed_ ? 0x0200 : 0x0100;
        }
        else if (feature == 4)
        {
            ++port.resets;
            reset_order_.push_back(port_index);
            if (port.child != nullptr && !port.hold_reset)
            {
                port.status = static_cast<uint16_t>((port.status | StatusEnable) & ~StatusReset);
                port.status = static_cast<uint16_t>((port.status & ~0x0600) | SpeedBits(port.speed));
                if (!port.skip_reset_change)
                {
                    port.change |= 0x0010;
                }
            }
        }
        return true;
    }
    if (request.request == 1)
    {
        clears_.emplace_back(port_index, feature);
        if (feature >= 16 && feature <= 20)
        {
            port.change &= static_cast<uint16_t>(~(1U << (feature - 16)));
        }
        else if (feature == 29)
        {
            port.change &= static_cast<uint16_t>(~0x0020U);
        }
        else if (feature == 25)
        {
            port.change &= static_cast<uint16_t>(~0x0040U);
        }
        else if (feature == 26)
        {
            port.change &= static_cast<uint16_t>(~0x0080U);
        }
        else if (feature == 1)
        {
            port.status &= static_cast<uint16_t>(~StatusEnable);
        }
        else if (feature == 8)
        {
            port.powered = false;
            port.status &= static_cast<uint16_t>(~(super_speed_ ? 0x0200 : 0x0100));
        }
        return true;
    }
    return false;
}

std::shared_ptr<ScriptedHub> MakeHub(Speed speed, uint8_t ports, bool multi_tt, uint8_t think_time)
{
    const bool super = speed >= Speed::Super;
    std::vector<uint8_t> device = MakeDeviceDescriptor(
        0x05E3, 0x0608, MaxPacket0Field(speed), 9, 2, super ? 0x0300 : (speed == Speed::High ? 0x0200 : 0x0110));
    device[6] = speed == Speed::High ? (multi_tt ? 2 : 1) : 0;
    auto hub = std::make_shared<ScriptedHub>(
        super, ports, std::move(device), super ? SuperSpeedHubConfiguration() : HubConfiguration(),
        think_time);
    hub->AddString(2, "Scripted Hub");
    return hub;
}

std::shared_ptr<ScriptedDevice> MakeMassStorage(Speed speed)
{
    auto device = std::make_shared<ScriptedDevice>(
        MakeDeviceDescriptor(0x46f4, 0x0001, MaxPacket0Field(speed)), MassStorageConfiguration());
    device->AddString(2, "QEMU USB HARDDRIVE");
    return device;
}

std::shared_ptr<ScriptedDevice> MakeKeyboard(Speed speed, uint8_t max_packet0)
{
    auto device = std::make_shared<ScriptedDevice>(
        MakeDeviceDescriptor(0x0627, 0x0001, max_packet0 != 0 ? max_packet0 : MaxPacket0Field(speed)),
        KeyboardConfiguration());
    device->AddString(2, "QEMU USB Keyboard");
    device->SetReportDescriptor(0, BootKeyboardDescriptor());
    return device;
}

std::shared_ptr<ScriptedDevice> MakeMouse(Speed speed, uint8_t max_packet0)
{
    auto device = std::make_shared<ScriptedDevice>(
        MakeDeviceDescriptor(0x0627, 0x0002, max_packet0 != 0 ? max_packet0 : MaxPacket0Field(speed)),
        MouseConfiguration());
    device->AddString(2, "QEMU USB Mouse");
    device->SetReportDescriptor(0, BootMouseDescriptor());
    return device;
}

std::shared_ptr<ScriptedDevice> MakeWheelMouse(Speed speed, bool boot_interface)
{
    static const std::vector<uint8_t> descriptor = {
        0x05, 0x01, 0x09, 0x02, 0xA1, 0x01, 0x05, 0x09, 0x19, 0x01, 0x29, 0x03, 0x15, 0x00,
        0x25, 0x01, 0x95, 0x03, 0x75, 0x01, 0x81, 0x02, 0x95, 0x01, 0x75, 0x05, 0x81, 0x01,
        0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x09, 0x38, 0x15, 0x81, 0x25, 0x7F, 0x75, 0x08,
        0x95, 0x03, 0x81, 0x06, 0x05, 0x0C, 0x0A, 0x38, 0x02, 0x95, 0x01, 0x81, 0x06, 0xC0};
    auto device = std::make_shared<ScriptedDevice>(
        MakeDeviceDescriptor(0x0627, 0x0003, MaxPacket0Field(speed)),
        GenericMouseConfiguration(static_cast<uint16_t>(descriptor.size()), boot_interface));
    device->AddString(2, "Wheel Mouse");
    device->SetReportDescriptor(0, descriptor);
    return device;
}

} // namespace FeatherOS::Usb::Testing
