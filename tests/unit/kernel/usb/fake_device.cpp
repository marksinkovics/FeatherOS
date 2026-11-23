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
        toggles_.clear();
        Configured();
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

void ScriptedDevice::Configured() {}

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

namespace
{

constexpr uint8_t OpTestUnitReady = 0x00;
constexpr uint8_t OpRequestSense = 0x03;
constexpr uint8_t OpInquiry = 0x12;
constexpr uint8_t OpModeSense6 = 0x1A;
constexpr uint8_t OpStartStop = 0x1B;
constexpr uint8_t OpReadCapacity10 = 0x25;
constexpr uint8_t OpRead10 = 0x28;
constexpr uint8_t OpWrite10 = 0x2A;
constexpr uint8_t OpSynchronizeCache = 0x35;
constexpr uint8_t OpRead16 = 0x88;
constexpr uint8_t OpWrite16 = 0x8A;
constexpr uint8_t OpServiceIn16 = 0x9E;
constexpr uint8_t BulkIn = 0x81;
constexpr uint8_t BulkOut = 0x02;
constexpr uint32_t CbwBytes = 31;

uint64_t BigEndian(const uint8_t* bytes, int count)
{
    uint64_t value = 0;
    for (int index = 0; index < count; ++index)
    {
        value = value << 8 | bytes[index];
    }
    return value;
}

void PutBigEndian(std::vector<uint8_t>& out, uint64_t value, int count)
{
    for (int index = count - 1; index >= 0; --index)
    {
        out.push_back(static_cast<uint8_t>(value >> (8 * index)));
    }
}

void PutLittleEndian(std::vector<uint8_t>& out, uint32_t value)
{
    for (int index = 0; index < 4; ++index)
    {
        out.push_back(static_cast<uint8_t>(value >> (8 * index)));
    }
}

uint32_t LittleEndian(std::span<const uint8_t> bytes, size_t at)
{
    uint32_t value = 0;
    for (size_t index = 0; index < 4; ++index)
    {
        value |= static_cast<uint32_t>(bytes[at + index]) << (8 * index);
    }
    return value;
}

void PutPadded(std::vector<uint8_t>& out, const std::string& text, size_t width)
{
    for (size_t index = 0; index < width; ++index)
    {
        out.push_back(index < text.size() ? static_cast<uint8_t>(text[index]) : ' ');
    }
}

bool IsRead(uint8_t opcode)
{
    return opcode == OpRead10 || opcode == OpRead16;
}

bool IsWrite(uint8_t opcode)
{
    return opcode == OpWrite10 || opcode == OpWrite16;
}

} // namespace

ScriptedMassStorage::ScriptedMassStorage(Speed speed, Model model)
    : ScriptedDevice(MakeDeviceDescriptor(0x46f4, 0x0001, MaxPacket0Field(speed)),
                     MassStorageConfiguration()),
      model_(std::move(model))
{
    AddString(2, "QEMU USB HARDDRIVE");
    image_.resize(model_.blocks * model_.block_size);
    for (size_t index = 0; index < image_.size(); ++index)
    {
        image_[index] = static_cast<uint8_t>(index * 31 + (index >> 9) * 7 + 5);
    }
}

std::vector<uint8_t>& ScriptedMassStorage::Image()
{
    return image_;
}

const ScriptedMassStorage::Model& ScriptedMassStorage::Params() const
{
    return model_;
}

void ScriptedMassStorage::FailStage(Stage stage, Fault fault, int count)
{
    if (stage == Stage::Command && fault != Fault::Unplug)
    {
        const Outcome outcome = fault == Fault::Stall  ? Outcome::Stall
                                : fault == Fault::Hang ? Outcome::Hang
                                                       : Outcome::TransactionError;
        FailEndpoint(BulkOut, outcome, count);
        return;
    }
    for (int index = 0; index < count; ++index)
    {
        stage_faults_[stage].push_back(fault);
    }
}

void ScriptedMassStorage::FailReply(Reply reply, int count)
{
    for (int index = 0; index < count; ++index)
    {
        replies_.push_back(reply);
    }
}

void ScriptedMassStorage::NotReady(int count)
{
    not_ready_ = count;
}

void ScriptedMassStorage::UnitAttention(uint8_t asc, int count)
{
    for (int index = 0; index < count; ++index)
    {
        unit_attentions_.push_back(asc);
    }
}

void ScriptedMassStorage::AbortedCommand(int count)
{
    aborted_ = count;
}

void ScriptedMassStorage::WriteProtect(bool protect)
{
    model_.write_protected = protect;
}

void ScriptedMassStorage::Configured()
{
    phase_ = Phase::Command;
    in_queue_.clear();
    received_.clear();
    hung_ = false;
}

void ScriptedMassStorage::NoMedium(bool missing)
{
    no_medium_ = missing;
}

void ScriptedMassStorage::SynchronizeCacheDataProtect(bool protect)
{
    sync_data_protect_ = protect;
}

void ScriptedMassStorage::SynchronizeCacheUnsupported(bool unsupported)
{
    sync_unsupported_ = unsupported;
}

void ScriptedMassStorage::ReportBlocks(uint64_t blocks)
{
    reported_blocks_ = blocks;
}

void ScriptedMassStorage::SetMaxLun(uint8_t lun, bool stall)
{
    max_lun_ = lun;
    stall_max_lun_ = stall;
}

const std::vector<ScriptedMassStorage::Logged>& ScriptedMassStorage::Commands() const
{
    return commands_;
}

uint32_t ScriptedMassStorage::BotResets() const
{
    return resets_;
}

bool ScriptedMassStorage::HandleClass(const Request& request, std::vector<uint8_t>* response,
                                      Outcome* outcome)
{
    if (request.type == 0xA1 && request.request == 0xFE)
    {
        if (stall_max_lun_)
        {
            *outcome = Outcome::Stall;
        }
        else
        {
            response->assign(1, max_lun_);
        }
        return true;
    }
    if (request.type == 0x21 && request.request == 0xFF)
    {
        ++resets_;
        phase_ = Phase::Command;
        in_queue_.clear();
        received_.clear();
        hung_ = false;
        return true;
    }
    return false;
}

bool ScriptedMassStorage::TakeStageFault(Stage stage, Fault* fault)
{
    std::deque<Fault>& queue = stage_faults_[stage];
    if (queue.empty())
    {
        return false;
    }
    *fault = queue.front();
    queue.pop_front();
    return true;
}

bool ScriptedMassStorage::TakeReply(Reply* reply)
{
    if (replies_.empty())
    {
        return false;
    }
    *reply = replies_.front();
    replies_.pop_front();
    return true;
}

std::vector<uint8_t> ScriptedMassStorage::Csw(uint32_t tag, uint8_t status, uint32_t residue)
{
    std::vector<uint8_t> bytes;
    PutLittleEndian(bytes, 0x53425355);
    PutLittleEndian(bytes, tag);
    PutLittleEndian(bytes, residue);
    bytes.push_back(status);
    return bytes;
}

void ScriptedMassStorage::CheckCondition(Answer* answer, const Cbw& cbw, uint8_t key, uint8_t asc,
                                         uint8_t ascq)
{
    sense_[0] = key;
    sense_[1] = asc;
    sense_[2] = ascq;
    answer->status = 1;
    answer->data.clear();
    answer->residue = cbw.length;
    answer->apply_write = false;
}

ScriptedMassStorage::Answer ScriptedMassStorage::Check(const Cbw& cbw)
{
    Answer answer;
    const uint8_t opcode = cbw.cdb[0];
    uint64_t lba = 0;
    uint32_t blocks = 0;
    if (opcode == OpRead10 || opcode == OpWrite10)
    {
        lba = BigEndian(&cbw.cdb[2], 4);
        blocks = static_cast<uint32_t>(BigEndian(&cbw.cdb[7], 2));
    }
    else if (opcode == OpRead16 || opcode == OpWrite16)
    {
        lba = BigEndian(&cbw.cdb[2], 8);
        blocks = static_cast<uint32_t>(BigEndian(&cbw.cdb[10], 4));
    }
    commands_.push_back({opcode, lba, blocks, cbw.tag});
    answer.lba = lba;
    answer.blocks = blocks;
    answer.residue = 0;

    if (opcode == OpInquiry)
    {
        std::vector<uint8_t> data = {model_.type, static_cast<uint8_t>(model_.removable ? 0x80 : 0),
                                     5, 2, 31, 0, 0, 0};
        PutPadded(data, model_.vendor, 8);
        PutPadded(data, model_.product, 16);
        PutPadded(data, "1.0", 4);
        data.resize(std::min<size_t>(data.size(), cbw.cdb[4]));
        answer.data = std::move(data);
        return answer;
    }
    if (opcode == OpRequestSense)
    {
        std::vector<uint8_t> data(18, 0);
        data[0] = 0x70;
        data[2] = sense_[0];
        data[7] = 10;
        data[12] = sense_[1];
        data[13] = sense_[2];
        sense_[0] = sense_[1] = sense_[2] = 0;
        data.resize(std::min<size_t>(data.size(), cbw.cdb[4]));
        answer.data = std::move(data);
        return answer;
    }
    if (!unit_attentions_.empty())
    {
        const uint8_t asc = unit_attentions_.front();
        unit_attentions_.pop_front();
        CheckCondition(&answer, cbw, 6, asc, 0);
        return answer;
    }
    if (no_medium_)
    {
        CheckCondition(&answer, cbw, 2, 0x3A, 0);
        return answer;
    }
    if (not_ready_ > 0)
    {
        --not_ready_;
        CheckCondition(&answer, cbw, 2, 0x04, 0x01);
        return answer;
    }
    if (aborted_ > 0)
    {
        --aborted_;
        CheckCondition(&answer, cbw, 0x0B, 0x00, 0);
        return answer;
    }
    switch (opcode)
    {
    case OpTestUnitReady:
        break;
    case OpReadCapacity10:
    {
        const uint64_t blocks_reported = reported_blocks_ != 0 ? reported_blocks_ : model_.blocks;
        PutBigEndian(answer.data, blocks_reported - 1 >= 0xFFFFFFFFULL ? 0xFFFFFFFFULL
                                                                        : blocks_reported - 1, 4);
        PutBigEndian(answer.data, model_.block_size, 4);
        break;
    }
    case OpServiceIn16:
    {
        const uint64_t blocks_reported = reported_blocks_ != 0 ? reported_blocks_ : model_.blocks;
        PutBigEndian(answer.data, blocks_reported - 1, 8);
        PutBigEndian(answer.data, model_.block_size, 4);
        answer.data.resize(32, 0);
        break;
    }
    case OpModeSense6:
        answer.data = {3, 0, static_cast<uint8_t>(model_.write_protected ? 0x80 : 0), 0};
        break;
    case OpSynchronizeCache:
        if (sync_data_protect_)
        {
            CheckCondition(&answer, cbw, 7, 0x27, 0);
        }
        else if (sync_unsupported_)
        {
            CheckCondition(&answer, cbw, 5, 0x20, 0);
        }
        break;
    case OpStartStop:
        ejected_ = true;
        break;
    case OpRead10:
    case OpRead16:
        if (lba + blocks > model_.blocks)
        {
            CheckCondition(&answer, cbw, 5, 0x21, 0);
            break;
        }
        answer.data.assign(image_.begin() + static_cast<long>(lba * model_.block_size),
                           image_.begin() + static_cast<long>((lba + blocks) * model_.block_size));
        break;
    case OpWrite10:
    case OpWrite16:
        if (model_.write_protected)
        {
            CheckCondition(&answer, cbw, 7, 0x27, 0);
        }
        else if (lba + blocks > model_.blocks)
        {
            CheckCondition(&answer, cbw, 5, 0x21, 0);
        }
        else
        {
            answer.apply_write = true;
        }
        break;
    default:
        CheckCondition(&answer, cbw, 5, 0x20, 0);
        break;
    }
    return answer;
}

void ScriptedMassStorage::QueueStatus(const Cbw& cbw, const Answer& answer)
{
    uint8_t status = answer.status;
    uint32_t tag = cbw.tag;
    uint32_t residue = answer.residue;
    Reply reply{};
    const bool faulty = TakeReply(&reply);
    std::vector<uint8_t> data = answer.data;
    if (faulty)
    {
        switch (reply)
        {
        case Reply::PhaseError:
            status = 2;
            break;
        case Reply::ShortData:
            data.resize(data.size() / 2);
            residue = cbw.in ? cbw.length - static_cast<uint32_t>(data.size()) : residue;
            break;
        case Reply::BadSignature:
            break;
        case Reply::BadTag:
            tag += 1;
            break;
        case Reply::ResidueLies:
            residue = cbw.length != 0 ? 1 : 0;
            break;
        }
    }
    if (cbw.in && !data.empty())
    {
        in_queue_.push_back({false, data});
    }
    else if (cbw.in && cbw.length != 0 && status == 1)
    {
        in_queue_.push_back({false, {}, true});
    }
    std::vector<uint8_t> csw = Csw(tag, status, residue);
    if (faulty && reply == Reply::BadSignature)
    {
        csw[0] ^= 1;
    }
    in_queue_.push_back({true, std::move(csw)});
}

void ScriptedMassStorage::Execute(const Cbw& cbw)
{
    Answer answer = Check(cbw);
    if (cbw.in && answer.status == 0)
    {
        const auto returned = static_cast<uint32_t>(answer.data.size());
        answer.residue = cbw.length > returned ? cbw.length - returned : 0;
        if (answer.data.size() > cbw.length)
        {
            answer.data.resize(cbw.length);
        }
    }
    if (cbw.in || cbw.length == 0)
    {
        QueueStatus(cbw, answer);
        return;
    }
    Fault fault{};
    if (TakeStageFault(Stage::Data, &fault))
    {
        switch (fault)
        {
        case Fault::Stall:
            FailEndpoint(BulkOut, Outcome::Stall, 1);
            answer.residue = cbw.length;
            answer.apply_write = false;
            QueueStatus(cbw, answer);
            return;
        case Fault::Hang:
            FailEndpoint(BulkOut, Outcome::Hang, 1);
            break;
        case Fault::TransactionError:
            FailEndpoint(BulkOut, Outcome::TransactionError, 1);
            break;
        case Fault::Unplug:
            if (on_unplug)
            {
                on_unplug();
            }
            break;
        }
    }
    phase_ = Phase::DataOut;
    pending_cbw_ = cbw;
    pending_answer_ = answer;
    received_.clear();
}

void ScriptedMassStorage::ReceiveBulk(uint8_t endpoint, std::span<const uint8_t> data)
{
    ScriptedDevice::ReceiveBulk(endpoint, data);
    if (endpoint != BulkOut)
    {
        return;
    }
    if (phase_ == Phase::DataOut)
    {
        received_.insert(received_.end(), data.begin(), data.end());
        if (received_.size() < pending_cbw_.length)
        {
            return;
        }
        Answer answer = pending_answer_;
        if (answer.apply_write)
        {
            std::copy(received_.begin(), received_.begin() + pending_cbw_.length,
                      image_.begin() + static_cast<long>(answer.lba * model_.block_size));
        }
        phase_ = Phase::Command;
        received_.clear();
        answer.residue = answer.status == 0 ? 0 : pending_cbw_.length;
        QueueStatus(pending_cbw_, answer);
        return;
    }
    if (data.size() != CbwBytes || LittleEndian(data, 0) != 0x43425355)
    {
        return;
    }
    Cbw cbw{};
    cbw.tag = LittleEndian(data, 4);
    cbw.length = LittleEndian(data, 8);
    cbw.in = (data[12] & 0x80U) != 0;
    std::copy(data.begin() + 15, data.begin() + 31, cbw.cdb);
    Fault fault{};
    if (TakeStageFault(Stage::Command, &fault) && fault == Fault::Unplug && on_unplug)
    {
        on_unplug();
    }
    Execute(cbw);
}

ScriptedDevice::Outcome ScriptedMassStorage::ProduceBulk(uint8_t endpoint, uint32_t max,
                                                         std::vector<uint8_t>* out)
{
    out->clear();
    if (endpoint != BulkIn || in_queue_.empty() || hung_)
    {
        return Outcome::Hang;
    }
    Item& item = in_queue_.front();
    if (item.stall)
    {
        in_queue_.pop_front();
        return Outcome::Stall;
    }
    Fault fault{};
    if (TakeStageFault(item.status ? Stage::Status : Stage::Data, &fault))
    {
        switch (fault)
        {
        case Fault::Stall:
            if (!item.status)
            {
                in_queue_.pop_front();
            }
            return Outcome::Stall;
        case Fault::Hang:
            hung_ = true;
            return Outcome::Hang;
        case Fault::TransactionError:
            return Outcome::TransactionError;
        case Fault::Unplug:
            if (on_unplug)
            {
                on_unplug();
            }
            hung_ = true;
            return Outcome::Hang;
        }
    }
    const size_t count = std::min<size_t>(max, item.bytes.size());
    out->assign(item.bytes.begin(), item.bytes.begin() + static_cast<long>(count));
    if (count == item.bytes.size())
    {
        in_queue_.pop_front();
    }
    else
    {
        item.bytes.erase(item.bytes.begin(), item.bytes.begin() + static_cast<long>(count));
    }
    return Outcome::Ok;
}

std::shared_ptr<ScriptedMassStorage> MakeDisk(Speed speed, ScriptedMassStorage::Model model)
{
    return std::make_shared<ScriptedMassStorage>(speed, std::move(model));
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
