#include "ethernet.h"

namespace FeatherOS::Network
{

bool ParseEthernetFrame(const uint8_t* frame, size_t length, EthernetFrame* parsed)
{
    if (frame == nullptr || length < EthernetHeaderSize || length > MaximumFrameSize)
    {
        return false;
    }
    for (size_t index = 0; index < 6; ++index)
    {
        parsed->destination.bytes[index] = frame[index];
        parsed->source.bytes[index] = frame[6 + index];
    }
    parsed->type = ReadBig16(frame + 12);
    parsed->payload = frame + EthernetHeaderSize;
    parsed->payload_length = length - EthernetHeaderSize;
    return true;
}

size_t BuildEthernetFrame(uint8_t (&frame)[MaximumFrameSize], const MacAddress& destination,
                          const MacAddress& source, uint16_t type, const uint8_t* payload,
                          size_t payload_length)
{
    if (payload_length > MaximumPayloadSize || (payload == nullptr && payload_length != 0))
    {
        return 0;
    }
    for (size_t index = 0; index < 6; ++index)
    {
        frame[index] = destination.bytes[index];
        frame[6 + index] = source.bytes[index];
    }
    WriteBig16(frame + 12, type);
    size_t length = EthernetHeaderSize;
    for (size_t index = 0; index < payload_length; ++index)
    {
        frame[length++] = payload[index];
    }
    while (length < MinimumFrameSize)
    {
        frame[length++] = 0;
    }
    return length;
}

} // namespace FeatherOS::Network
