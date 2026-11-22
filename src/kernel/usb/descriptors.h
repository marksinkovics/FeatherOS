#ifndef FEATHEROS_USB_DESCRIPTORS_H
#define FEATHEROS_USB_DESCRIPTORS_H

#include "kernel/usb/host_controller.h"

#include <span>
#include <stddef.h>
#include <stdint.h>

namespace FeatherOS::Usb
{

constexpr uint32_t MaxConfigurationBytes = 4096;
constexpr uint32_t MaxInterfaces = 8;
constexpr uint32_t MaxInterfaceEndpoints = 8;
constexpr uint32_t MaxClassDescriptors = 2;
constexpr uint32_t MaxClassDescriptorBytes = 16;
constexpr uint32_t DeviceDescriptorBytes = 18;
constexpr uint32_t ConfigurationHeaderBytes = 9;

namespace DescriptorType
{
constexpr uint8_t Device = 1;
constexpr uint8_t Configuration = 2;
constexpr uint8_t String = 3;
constexpr uint8_t Interface = 4;
constexpr uint8_t Endpoint = 5;
constexpr uint8_t Hid = 0x21;
constexpr uint8_t HidReport = 0x22;
constexpr uint8_t Hub = 0x29;
constexpr uint8_t SuperSpeedHub = 0x2A;
constexpr uint8_t SuperSpeedCompanion = 0x30;
} // namespace DescriptorType

struct DeviceDescriptor
{
    uint16_t usb_version;
    uint8_t device_class;
    uint8_t device_subclass;
    uint8_t device_protocol;
    uint8_t max_packet_size0;
    uint16_t vendor;
    uint16_t product;
    uint16_t device_version;
    uint8_t manufacturer_index;
    uint8_t product_index;
    uint8_t serial_index;
    uint8_t configuration_count;
};

// A class-specific descriptor found inside an interface; bytes holds at most the first
// MaxClassDescriptorBytes of the descriptor, length its real size.
struct ClassDescriptor
{
    uint8_t type;
    uint8_t length;
    uint8_t bytes[MaxClassDescriptorBytes];
};

struct InterfaceView
{
    uint8_t number;
    uint8_t class_code;
    uint8_t subclass;
    uint8_t protocol;
    uint8_t endpoint_count;
    uint8_t class_descriptor_count;
    EndpointDescriptor endpoints[MaxInterfaceEndpoints];
    ClassDescriptor class_descriptors[MaxClassDescriptors];
};

// Only the interfaces' alternate setting 0 is listed.
struct ConfigurationView
{
    uint8_t value;
    uint8_t attributes;
    uint8_t max_power;
    uint16_t total_length;
    uint8_t interface_count;
    InterfaceView interfaces[MaxInterfaces];
};

enum class ParseStatus : uint8_t
{
    Ok,
    Truncated,
    Malformed,
    TooLarge,
};

ParseStatus ParseDevice(std::span<const uint8_t> bytes, DeviceDescriptor* device);
// Reads nothing past wTotalLength or the buffer; a descriptor shorter than 2 bytes or reaching
// beyond wTotalLength is malformed.
ParseStatus ParseConfiguration(std::span<const uint8_t> bytes, ConfigurationView* configuration);
// The length of the HID report descriptor the interface's HID descriptor announces, else 0.
uint16_t HidReportLength(const InterfaceView& interface);
// Converts a string descriptor to ASCII ('?' for anything else); false if it is not one.
bool DecodeString(std::span<const uint8_t> bytes, char* text, size_t capacity);

} // namespace FeatherOS::Usb

#endif
