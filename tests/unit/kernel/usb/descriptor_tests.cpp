#include <catch2/catch_test_macros.hpp>

#include "fake_device.h"

#include "kernel/usb/descriptors.h"

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace
{

using namespace FeatherOS::Usb;
using namespace FeatherOS::Usb::Testing;

ParseStatus Parse(const std::vector<uint8_t>& bytes, ConfigurationView* view)
{
    return ParseConfiguration(std::span<const uint8_t>(bytes.data(), bytes.size()), view);
}

void SetTotal(std::vector<uint8_t>& bytes, uint32_t total)
{
    bytes[2] = static_cast<uint8_t>(total);
    bytes[3] = static_cast<uint8_t>(total >> 8);
}

TEST_CASE("descriptors T18: a keyboard has one HID interface with an interrupt IN endpoint",
          "[usb][descriptors]")
{
    ConfigurationView view;
    REQUIRE(Parse(KeyboardConfiguration(), &view) == ParseStatus::Ok);
    REQUIRE(view.value == 1);
    REQUIRE(view.interface_count == 1);
    const InterfaceView& keyboard = view.interfaces[0];
    REQUIRE(keyboard.class_code == 3);
    REQUIRE(keyboard.subclass == 1);
    REQUIRE(keyboard.protocol == 1);
    REQUIRE(keyboard.endpoint_count == 1);
    REQUIRE(keyboard.endpoints[0].address == 0x81);
    REQUIRE(keyboard.endpoints[0].type == EndpointType::Interrupt);
    REQUIRE(keyboard.endpoints[0].max_packet_size == 8);
    REQUIRE(keyboard.endpoints[0].interval == 10);
    REQUIRE(keyboard.class_descriptor_count == 1);
    REQUIRE(keyboard.class_descriptors[0].type == DescriptorType::Hid);
    REQUIRE(HidReportLength(keyboard) == 63);
}

TEST_CASE("descriptors T18: a mouse and a keyboard-and-mouse combination", "[usb][descriptors]")
{
    ConfigurationView view;
    REQUIRE(Parse(MouseConfiguration(), &view) == ParseStatus::Ok);
    REQUIRE(view.interface_count == 1);
    REQUIRE(view.interfaces[0].protocol == 2);
    REQUIRE(view.interfaces[0].endpoints[0].max_packet_size == 4);
    REQUIRE(HidReportLength(view.interfaces[0]) == 52);

    REQUIRE(Parse(ComboConfiguration(), &view) == ParseStatus::Ok);
    REQUIRE(view.interface_count == 2);
    REQUIRE(view.interfaces[0].number == 0);
    REQUIRE(view.interfaces[0].protocol == 1);
    REQUIRE(view.interfaces[0].endpoints[0].address == 0x81);
    REQUIRE(view.interfaces[1].number == 1);
    REQUIRE(view.interfaces[1].protocol == 2);
    REQUIRE(view.interfaces[1].endpoints[0].address == 0x82);
    REQUIRE(HidReportLength(view.interfaces[1]) == 52);
}

TEST_CASE("descriptors T18: a hub lists its alternate setting 0 only", "[usb][descriptors]")
{
    ConfigurationView view;
    REQUIRE(Parse(HubConfiguration(), &view) == ParseStatus::Ok);
    REQUIRE(view.interface_count == 1);
    REQUIRE(view.interfaces[0].class_code == 9);
    REQUIRE(view.interfaces[0].protocol == 1);
    REQUIRE(view.interfaces[0].endpoint_count == 1);
    REQUIRE(view.interfaces[0].endpoints[0].max_packet_size == 1);
    REQUIRE(view.interfaces[0].endpoints[0].interval == 12);
}

TEST_CASE("descriptors T18: mass storage keeps its bulk endpoints and their companions",
          "[usb][descriptors]")
{
    ConfigurationView view;
    REQUIRE(Parse(MassStorageConfiguration(), &view) == ParseStatus::Ok);
    REQUIRE(view.interface_count == 1);
    const InterfaceView& storage = view.interfaces[0];
    REQUIRE(storage.class_code == 8);
    REQUIRE(storage.protocol == 0x50);
    REQUIRE(storage.endpoint_count == 2);
    REQUIRE(storage.endpoints[0].address == 0x81);
    REQUIRE(storage.endpoints[0].type == EndpointType::Bulk);
    REQUIRE(storage.endpoints[0].max_packet_size == 512);
    REQUIRE(storage.endpoints[0].max_burst == 15);
    REQUIRE(storage.endpoints[1].address == 0x02);
    REQUIRE(storage.endpoints[1].type == EndpointType::Bulk);
    REQUIRE(storage.endpoints[1].max_burst == 15);
}

TEST_CASE("descriptors T18: a hub class descriptor inside an interface is kept",
          "[usb][descriptors]")
{
    std::vector<uint8_t> bytes = HubConfiguration();
    const std::vector<uint8_t> hub = {9, 0x29, 4, 0x09, 0, 50, 100, 0, 0xFF};
    bytes.insert(bytes.begin() + 18, hub.begin(), hub.end());
    SetTotal(bytes, static_cast<uint32_t>(bytes.size()));
    ConfigurationView view;
    REQUIRE(Parse(bytes, &view) == ParseStatus::Ok);
    REQUIRE(view.interfaces[0].class_descriptor_count == 1);
    REQUIRE(view.interfaces[0].class_descriptors[0].type == DescriptorType::Hub);
    REQUIRE(view.interfaces[0].class_descriptors[0].bytes[2] == 4);
}

TEST_CASE("descriptors T18: a buffer cut short is refused, never read past", "[usb][descriptors]")
{
    const std::vector<uint8_t> whole = KeyboardConfiguration();
    for (size_t size = 0; size < whole.size(); ++size)
    {
        // Each copy is exactly size bytes, so a read past the end is a sanitizer failure.
        const std::vector<uint8_t> cut(whole.begin(), whole.begin() + static_cast<long>(size));
        ConfigurationView view;
        REQUIRE(Parse(cut, &view) == ParseStatus::Truncated);
    }
}

TEST_CASE("descriptors T18: wTotalLength larger than the buffer or 4 KiB", "[usb][descriptors]")
{
    std::vector<uint8_t> bytes = KeyboardConfiguration();
    SetTotal(bytes, static_cast<uint32_t>(bytes.size()) + 1);
    ConfigurationView view;
    REQUIRE(Parse(bytes, &view) == ParseStatus::Truncated);

    bytes = KeyboardConfiguration();
    SetTotal(bytes, 4097);
    bytes.resize(5000);
    REQUIRE(Parse(bytes, &view) == ParseStatus::TooLarge);

    SetTotal(bytes, 4096);
    for (size_t index = 34; index < bytes.size(); ++index)
    {
        bytes[index] = 0;
    }
    bytes[34] = 2;
    bytes[35] = 0xFE;
    size_t at = 36;
    while (at + 2 <= 4096)
    {
        bytes[at] = 2;
        bytes[at + 1] = 0xFE;
        at += 2;
    }
    REQUIRE(Parse(bytes, &view) == ParseStatus::Ok);
    REQUIRE(view.total_length == 4096);
}

TEST_CASE("descriptors T18: a descriptor shorter than 2 bytes is malformed", "[usb][descriptors]")
{
    for (const uint8_t length : {0, 1})
    {
        // Known types are caught by their own minimum, so the header check is what refuses a
        // descriptor of a type the parser skips.
        for (const size_t at : {size_t{9}, size_t{18}, size_t{27}})
        {
            std::vector<uint8_t> bytes = KeyboardConfiguration();
            // The byte after it is a valid descriptor, so only the length check can refuse this.
            bytes.insert(bytes.begin() + static_cast<long>(at), length);
            SetTotal(bytes, static_cast<uint32_t>(bytes.size()));
            ConfigurationView view;
            REQUIRE(Parse(bytes, &view) == ParseStatus::Malformed);
        }

        std::vector<uint8_t> bytes = KeyboardConfiguration();
        bytes[9] = length;
        ConfigurationView view;
        REQUIRE(Parse(bytes, &view) == ParseStatus::Malformed);

        bytes = KeyboardConfiguration();
        bytes[bytes.size() - 7] = length;
        REQUIRE(Parse(bytes, &view) == ParseStatus::Malformed);
    }
}

TEST_CASE("descriptors T18: a descriptor reaching past wTotalLength is malformed",
          "[usb][descriptors]")
{
    std::vector<uint8_t> bytes = KeyboardConfiguration();
    // The last descriptor is the 7-byte endpoint; cut the total in the middle of it.
    SetTotal(bytes, static_cast<uint32_t>(bytes.size()) - 3);
    ConfigurationView view;
    REQUIRE(Parse(bytes, &view) == ParseStatus::Malformed);

    // A lone trailing byte cannot hold a descriptor header.
    bytes = KeyboardConfiguration();
    bytes.push_back(7);
    SetTotal(bytes, static_cast<uint32_t>(bytes.size()));
    REQUIRE(Parse(bytes, &view) == ParseStatus::Malformed);
}

TEST_CASE("descriptors T18: descriptors too short for their type are malformed",
          "[usb][descriptors]")
{
    ConfigurationView view;
    std::vector<uint8_t> bytes = KeyboardConfiguration();
    bytes[9] = 8;
    REQUIRE(Parse(bytes, &view) == ParseStatus::Malformed);

    bytes = KeyboardConfiguration();
    bytes[bytes.size() - 7] = 5;
    REQUIRE(Parse(bytes, &view) == ParseStatus::Malformed);

    bytes = KeyboardConfiguration();
    bytes[0] = 8;
    REQUIRE(Parse(bytes, &view) == ParseStatus::Malformed);
    bytes[0] = 9;
    bytes[1] = 1;
    REQUIRE(Parse(bytes, &view) == ParseStatus::Malformed);
}

TEST_CASE("descriptors T18: more interfaces or endpoints than fit are dropped, not overrun",
          "[usb][descriptors]")
{
    std::vector<uint8_t> bytes = {9, 2, 0, 0, 12, 1, 0, 0xA0, 50};
    for (uint8_t number = 0; number < 12; ++number)
    {
        const std::vector<uint8_t> interface = {9, 4, number, 0, 10, 3, 1, 1, 0};
        bytes.insert(bytes.end(), interface.begin(), interface.end());
        for (uint8_t endpoint = 0; endpoint < 10; ++endpoint)
        {
            const std::vector<uint8_t> entry = {7, 5, static_cast<uint8_t>(0x81 + endpoint), 3, 8, 0, 10};
            bytes.insert(bytes.end(), entry.begin(), entry.end());
        }
    }
    SetTotal(bytes, static_cast<uint32_t>(bytes.size()));
    ConfigurationView view;
    REQUIRE(Parse(bytes, &view) == ParseStatus::Ok);
    REQUIRE(view.interface_count == MaxInterfaces);
    REQUIRE(view.interfaces[0].endpoint_count == MaxInterfaceEndpoints);
}

TEST_CASE("descriptors: the device descriptor and string descriptors", "[usb][descriptors]")
{
    DeviceDescriptor device;
    std::vector<uint8_t> bytes = MakeDeviceDescriptor(0x0627, 0x0001, 64, 0, 2, 0x0210);
    REQUIRE(ParseDevice(bytes, &device) == ParseStatus::Ok);
    REQUIRE(device.vendor == 0x0627);
    REQUIRE(device.product == 0x0001);
    REQUIRE(device.max_packet_size0 == 64);
    REQUIRE(device.usb_version == 0x0210);
    REQUIRE(device.product_index == 2);
    REQUIRE(device.configuration_count == 1);
    bytes.pop_back();
    REQUIRE(ParseDevice(bytes, &device) == ParseStatus::Truncated);
    bytes = MakeDeviceDescriptor(1, 2, 8);
    bytes[1] = 2;
    REQUIRE(ParseDevice(bytes, &device) == ParseStatus::Malformed);

    char text[16];
    const std::vector<uint8_t> name = {12, 3, 'F', 0, 'e', 0, 0xAC, 0x20, 'r', 0};
    REQUIRE_FALSE(DecodeString(name, text, sizeof(text)));
    const std::vector<uint8_t> good = {10, 3, 'F', 0, 'e', 0, 0xAC, 0x20, 'r', 0};
    REQUIRE(DecodeString(good, text, sizeof(text)));
    REQUIRE(std::string(text) == "Fe?r");
    char tiny[3];
    REQUIRE(DecodeString(good, tiny, sizeof(tiny)));
    REQUIRE(std::string(tiny) == "Fe");
    REQUIRE_FALSE(DecodeString(std::vector<uint8_t>{4, 2, 0, 0}, text, sizeof(text)));
}

} // namespace
