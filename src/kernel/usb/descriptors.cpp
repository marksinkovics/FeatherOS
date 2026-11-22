#include "descriptors.h"

namespace FeatherOS::Usb
{
namespace
{

constexpr uint32_t InterfaceBytes = 9;
constexpr uint32_t EndpointBytes = 7;
constexpr uint32_t CompanionBytes = 6;
constexpr uint32_t HidReportEntries = 6;

uint16_t Word(std::span<const uint8_t> bytes, size_t offset)
{
    return static_cast<uint16_t>(bytes[offset] | (bytes[offset + 1] << 8));
}

EndpointType TypeFromAttributes(uint8_t attributes)
{
    switch (attributes & 3U)
    {
    case 0:
        return EndpointType::Control;
    case 1:
        return EndpointType::Isochronous;
    case 2:
        return EndpointType::Bulk;
    default:
        break;
    }
    return EndpointType::Interrupt;
}

} // namespace

ParseStatus ParseDevice(std::span<const uint8_t> bytes, DeviceDescriptor* device)
{
    if (bytes.size() < DeviceDescriptorBytes)
    {
        return ParseStatus::Truncated;
    }
    if (bytes[0] < DeviceDescriptorBytes || bytes[1] != DescriptorType::Device)
    {
        return ParseStatus::Malformed;
    }
    device->usb_version = Word(bytes, 2);
    device->device_class = bytes[4];
    device->device_subclass = bytes[5];
    device->device_protocol = bytes[6];
    device->max_packet_size0 = bytes[7];
    device->vendor = Word(bytes, 8);
    device->product = Word(bytes, 10);
    device->device_version = Word(bytes, 12);
    device->manufacturer_index = bytes[14];
    device->product_index = bytes[15];
    device->serial_index = bytes[16];
    device->configuration_count = bytes[17];
    return ParseStatus::Ok;
}

ParseStatus ParseConfiguration(std::span<const uint8_t> bytes, ConfigurationView* configuration)
{
    *configuration = {};
    if (bytes.size() < ConfigurationHeaderBytes)
    {
        return ParseStatus::Truncated;
    }
    if (bytes[0] < ConfigurationHeaderBytes || bytes[1] != DescriptorType::Configuration)
    {
        return ParseStatus::Malformed;
    }
    const uint32_t total = Word(bytes, 2);
    if (total < bytes[0])
    {
        return ParseStatus::Malformed;
    }
    if (total > MaxConfigurationBytes)
    {
        return ParseStatus::TooLarge;
    }
    if (total > bytes.size())
    {
        return ParseStatus::Truncated;
    }
    configuration->total_length = static_cast<uint16_t>(total);
    configuration->value = bytes[5];
    configuration->attributes = bytes[7];
    configuration->max_power = bytes[8];

    InterfaceView* current = nullptr;
    EndpointDescriptor* last_endpoint = nullptr;
    size_t offset = bytes[0];
    while (offset < total)
    {
        if (offset + 2 > total)
        {
            return ParseStatus::Malformed;
        }
        const uint32_t length = bytes[offset];
        const uint8_t type = bytes[offset + 1];
        if (length < 2 || offset + length > total)
        {
            return ParseStatus::Malformed;
        }
        switch (type)
        {
        case DescriptorType::Interface:
            if (length < InterfaceBytes)
            {
                return ParseStatus::Malformed;
            }
            current = nullptr;
            last_endpoint = nullptr;
            if (bytes[offset + 3] == 0 && configuration->interface_count < MaxInterfaces)
            {
                current = &configuration->interfaces[configuration->interface_count++];
                current->number = bytes[offset + 2];
                current->class_code = bytes[offset + 5];
                current->subclass = bytes[offset + 6];
                current->protocol = bytes[offset + 7];
            }
            break;
        case DescriptorType::Endpoint:
            if (length < EndpointBytes)
            {
                return ParseStatus::Malformed;
            }
            last_endpoint = nullptr;
            if (current != nullptr && current->endpoint_count < MaxInterfaceEndpoints)
            {
                last_endpoint = &current->endpoints[current->endpoint_count++];
                *last_endpoint = {};
                last_endpoint->address = bytes[offset + 2];
                last_endpoint->type = TypeFromAttributes(bytes[offset + 3]);
                last_endpoint->max_packet_size = Word(bytes, offset + 4);
                last_endpoint->interval = bytes[offset + 6];
            }
            break;
        case DescriptorType::SuperSpeedCompanion:
            if (length < CompanionBytes)
            {
                return ParseStatus::Malformed;
            }
            if (last_endpoint != nullptr)
            {
                last_endpoint->max_burst = bytes[offset + 2];
                last_endpoint->mult = bytes[offset + 3] & 3U;
                last_endpoint->bytes_per_interval = Word(bytes, offset + 4);
            }
            break;
        case DescriptorType::Hid:
        case DescriptorType::Hub:
        case DescriptorType::SuperSpeedHub:
            if (current != nullptr && current->class_descriptor_count < MaxClassDescriptors)
            {
                ClassDescriptor& entry =
                    current->class_descriptors[current->class_descriptor_count++];
                entry.type = type;
                entry.length = static_cast<uint8_t>(length);
                for (uint32_t index = 0; index < length && index < MaxClassDescriptorBytes; ++index)
                {
                    entry.bytes[index] = bytes[offset + index];
                }
            }
            break;
        default:
            break;
        }
        offset += length;
    }
    return ParseStatus::Ok;
}

uint16_t HidReportLength(const InterfaceView& interface)
{
    for (uint32_t index = 0; index < interface.class_descriptor_count; ++index)
    {
        const ClassDescriptor& entry = interface.class_descriptors[index];
        if (entry.type != DescriptorType::Hid)
        {
            continue;
        }
        const uint32_t available =
            entry.length < MaxClassDescriptorBytes ? entry.length : MaxClassDescriptorBytes;
        for (uint32_t at = HidReportEntries; at + 3 <= available; at += 3)
        {
            if (entry.bytes[at] == DescriptorType::HidReport)
            {
                return static_cast<uint16_t>(entry.bytes[at + 1] | (entry.bytes[at + 2] << 8));
            }
        }
    }
    return 0;
}

bool DecodeString(std::span<const uint8_t> bytes, char* text, size_t capacity)
{
    if (capacity == 0)
    {
        return false;
    }
    text[0] = '\0';
    if (bytes.size() < 2 || bytes[0] < 2 || bytes[0] > bytes.size() ||
        bytes[1] != DescriptorType::String)
    {
        return false;
    }
    size_t out = 0;
    for (size_t offset = 2; offset + 2 <= bytes[0] && out + 1 < capacity; offset += 2)
    {
        const uint16_t unit = Word(bytes, offset);
        text[out++] = unit >= 0x20 && unit < 0x7F ? static_cast<char>(unit) : '?';
    }
    text[out] = '\0';
    return true;
}

} // namespace FeatherOS::Usb
