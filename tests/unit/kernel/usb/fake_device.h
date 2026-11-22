#ifndef FEATHEROS_TESTS_USB_FAKE_DEVICE_H
#define FEATHEROS_TESTS_USB_FAKE_DEVICE_H

#include "kernel/usb/host_controller.h"

#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace FeatherOS::Usb::Testing
{

// A device behind a root port: it answers control requests from its descriptors, hands out
// queued reports on interrupt IN endpoints, and fails on demand.
class ScriptedDevice
{
public:
    enum class Outcome
    {
        Ok,
        Stall,
        Hang,
        TransactionError,
        Babble,
    };

    struct Request
    {
        uint8_t type;
        uint8_t request;
        uint16_t value;
        uint16_t index;
        uint16_t length;
    };

    ScriptedDevice(std::vector<uint8_t> device, std::vector<uint8_t> configuration);

    void AddString(uint8_t index, const std::string& text);
    // Descriptor length 0: bMaxPacketSize0 as bytes; a SuperSpeed device writes the exponent.
    uint32_t MaxPacket0(Speed speed) const;

    Outcome Control(const Request& request, std::vector<uint8_t>* response);
    // The next count requests (of one request code, or any) end this way.
    void FailControl(Outcome outcome, int count, int request = -1);

    void QueueReport(uint8_t endpoint, std::vector<uint8_t> report);
    bool TakeReport(uint8_t endpoint, std::vector<uint8_t>* report);
    bool HasReport(uint8_t endpoint) const;
    void FailEndpoint(uint8_t endpoint, Outcome outcome, int count);
    Outcome EndpointOutcome(uint8_t endpoint);

    // Called as each control request arrives, before it is answered.
    std::function<void(const Request&)> on_request;

    const std::vector<Request>& Requests() const;
    uint32_t Count(uint8_t type, uint8_t request) const;
    uint8_t configuration() const;

private:
    struct Fault
    {
        Outcome outcome;
        int remaining;
        int request;
    };

    std::vector<uint8_t> device_;
    std::vector<uint8_t> configuration_;
    std::map<uint8_t, std::vector<uint8_t>> strings_;
    std::vector<Request> requests_;
    std::vector<Fault> control_faults_;
    std::map<uint8_t, std::deque<std::vector<uint8_t>>> reports_;
    std::map<uint8_t, std::vector<Fault>> endpoint_faults_;
    uint8_t configuration_value_ = 0;
};

std::vector<uint8_t> MakeDeviceDescriptor(uint16_t vendor, uint16_t product, uint8_t max_packet0,
                                           uint8_t device_class = 0, uint8_t product_string = 2,
                                           uint16_t usb_version = 0x0200);
std::vector<uint8_t> KeyboardConfiguration();
std::vector<uint8_t> MouseConfiguration();
std::vector<uint8_t> ComboConfiguration();
std::vector<uint8_t> HubConfiguration();
std::vector<uint8_t> MassStorageConfiguration();

// bMaxPacketSize0 a device of this speed reports (the exponent at SuperSpeed).
uint8_t MaxPacket0Field(Speed speed);

std::shared_ptr<ScriptedDevice> MakeKeyboard(Speed speed, uint8_t max_packet0 = 0);
std::shared_ptr<ScriptedDevice> MakeMouse(Speed speed, uint8_t max_packet0 = 0);

} // namespace FeatherOS::Usb::Testing

#endif
