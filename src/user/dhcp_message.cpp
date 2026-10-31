#include "dhcp_message.h"

namespace FeatherOS::User::Dhcp
{
namespace
{

constexpr uint8_t BootRequest = 1;
constexpr uint8_t BootReply = 2;
constexpr uint8_t EthernetType = 1;
constexpr size_t OptionsOffset = 240;
constexpr uint8_t MagicCookie[4] = {99, 130, 83, 99};
constexpr uint8_t PadOption = 0;
constexpr uint8_t NetmaskOption = 1;
constexpr uint8_t RouterOption = 3;
constexpr uint8_t NameServerOption = 6;
constexpr uint8_t RequestedAddressOption = 50;
constexpr uint8_t LeaseTimeOption = 51;
constexpr uint8_t MessageTypeOption = 53;
constexpr uint8_t ServerIdentifierOption = 54;
constexpr uint8_t ParameterListOption = 55;
constexpr uint8_t RenewalTimeOption = 58;
constexpr uint8_t RebindingTimeOption = 59;
constexpr uint8_t EndOption = 255;

void Copy4(uint8_t* destination, const uint8_t* source)
{
    for (size_t index = 0; index < 4; ++index)
    {
        destination[index] = source[index];
    }
}

uint32_t ReadBig32(const uint8_t* data)
{
    return (static_cast<uint32_t>(data[0]) << 24) | (static_cast<uint32_t>(data[1]) << 16) |
           (static_cast<uint32_t>(data[2]) << 8) | data[3];
}

size_t AppendAddressOption(uint8_t* message, size_t offset, uint8_t code, const uint8_t* address)
{
    message[offset] = code;
    message[offset + 1] = 4;
    Copy4(message + offset + 2, address);
    return offset + 6;
}

size_t Build(uint8_t (&message)[MessageSize], uint8_t type, uint32_t transaction,
             const uint8_t (&mac)[6], const uint8_t* client_address, const uint8_t* requested,
             const uint8_t* server)
{
    for (uint8_t& byte : message)
    {
        byte = 0;
    }
    message[0] = BootRequest;
    message[1] = EthernetType;
    message[2] = sizeof(mac);
    for (size_t index = 0; index < 4; ++index)
    {
        message[4 + index] = static_cast<uint8_t>(transaction >> (24 - index * 8));
    }
    if (client_address == nullptr)
    {
        // The broadcast flag.
        message[10] = 0x80;
    }
    else
    {
        Copy4(message + 12, client_address);
    }
    for (size_t index = 0; index < sizeof(mac); ++index)
    {
        message[28 + index] = mac[index];
    }
    Copy4(message + 236, MagicCookie);

    size_t offset = OptionsOffset;
    message[offset++] = MessageTypeOption;
    message[offset++] = 1;
    message[offset++] = type;
    if (requested != nullptr)
    {
        offset = AppendAddressOption(message, offset, RequestedAddressOption, requested);
    }
    if (server != nullptr)
    {
        offset = AppendAddressOption(message, offset, ServerIdentifierOption, server);
    }
    message[offset++] = ParameterListOption;
    message[offset++] = 4;
    message[offset++] = NetmaskOption;
    message[offset++] = RouterOption;
    message[offset++] = NameServerOption;
    message[offset++] = LeaseTimeOption;
    message[offset] = EndOption;
    return MessageSize;
}

} // namespace

size_t BuildMessage(uint8_t (&message)[MessageSize], uint8_t type, uint32_t transaction,
                    const uint8_t (&mac)[6], const uint8_t* requested, const uint8_t* server)
{
    return Build(message, type, transaction, mac, nullptr, requested, server);
}

size_t BuildRenewal(uint8_t (&message)[MessageSize], uint32_t transaction, const uint8_t (&mac)[6],
                    const uint8_t (&client_address)[4])
{
    return Build(message, Request, transaction, mac, client_address, nullptr, nullptr);
}

bool ParseReply(const uint8_t* message, size_t length, uint32_t transaction,
                const uint8_t (&mac)[6], Reply* reply)
{
    if (message == nullptr || reply == nullptr || length < OptionsOffset ||
        message[0] != BootReply || message[1] != EthernetType || message[2] != sizeof(mac) ||
        ReadBig32(message + 4) != transaction || ReadBig32(message + 236) != ReadBig32(MagicCookie))
    {
        return false;
    }
    for (size_t index = 0; index < sizeof(mac); ++index)
    {
        if (message[28 + index] != mac[index])
        {
            return false;
        }
    }
    *reply = {};
    Copy4(reply->your_address, message + 16);
    for (size_t offset = OptionsOffset; offset < length;)
    {
        const uint8_t code = message[offset];
        if (code == EndOption)
        {
            break;
        }
        if (code == PadOption)
        {
            ++offset;
            continue;
        }
        if (offset + 2 > length || offset + 2 + message[offset + 1] > length)
        {
            return false;
        }
        const uint8_t size = message[offset + 1];
        const uint8_t* data = message + offset + 2;
        if (code == MessageTypeOption && size == 1)
        {
            reply->type = data[0];
        }
        else if (code == ServerIdentifierOption && size == 4)
        {
            Copy4(reply->server, data);
        }
        else if (code == NetmaskOption && size == 4)
        {
            reply->has_netmask = true;
            Copy4(reply->netmask, data);
        }
        else if (code == RouterOption && size >= 4 && size % 4 == 0)
        {
            reply->has_router = true;
            Copy4(reply->router, data);
        }
        else if (code == NameServerOption && size >= 4 && size % 4 == 0)
        {
            for (size_t index = 0;
                 index < size / 4U && reply->name_server_count < MaximumNameServers; ++index)
            {
                Copy4(reply->name_servers[reply->name_server_count++], data + index * 4);
            }
        }
        else if (code == LeaseTimeOption && size == 4)
        {
            reply->lease_seconds = ReadBig32(data);
        }
        else if (code == RenewalTimeOption && size == 4)
        {
            reply->renewal_seconds = ReadBig32(data);
        }
        else if (code == RebindingTimeOption && size == 4)
        {
            reply->rebinding_seconds = ReadBig32(data);
        }
        offset += 2 + size;
    }
    return reply->type != 0;
}

int NetmaskPrefixLength(const uint8_t (&netmask)[4])
{
    const uint32_t mask = ReadBig32(netmask);
    int length = 0;
    while (length < 32 && (mask & (0x80000000U >> length)) != 0)
    {
        ++length;
    }
    const uint32_t expected = length == 0 ? 0 : 0xFFFFFFFFU << (32 - length);
    return mask == expected ? length : -1;
}

} // namespace FeatherOS::User::Dhcp
