#include "tcp.h"

#include "address.h"

namespace FeatherOS::Network
{
namespace
{

constexpr uint8_t OptionEnd = 0;
constexpr uint8_t OptionNoOperation = 1;
constexpr uint8_t OptionMaximumSegmentSize = 2;
constexpr uint8_t OptionWindowScale = 3;

// A malformed option ends the list, keeping what came before it.
void ReadOptions(const uint8_t* options, size_t length, TcpSegment* segment)
{
    segment->maximum_segment_size = 0;
    segment->has_window_scale = false;
    segment->window_scale = 0;
    size_t offset = 0;
    while (offset < length && options[offset] != OptionEnd)
    {
        if (options[offset] == OptionNoOperation)
        {
            ++offset;
            continue;
        }
        if (offset + 1 >= length || options[offset + 1] < 2 ||
            options[offset + 1] > length - offset)
        {
            return;
        }
        const uint8_t kind = options[offset];
        const uint8_t size = options[offset + 1];
        if (kind == OptionMaximumSegmentSize && size == 4)
        {
            segment->maximum_segment_size = ReadBig16(options + offset + 2);
        }
        else if (kind == OptionWindowScale && size == 3)
        {
            segment->has_window_scale = true;
            segment->window_scale = options[offset + 2] < TcpMaximumWindowScale
                                        ? options[offset + 2]
                                        : TcpMaximumWindowScale;
        }
        offset += size;
    }
}

} // namespace

bool ParseTcpSegment(const uint8_t* data, size_t length, TcpSegment* segment)
{
    if (data == nullptr || segment == nullptr || length < TcpHeaderSize)
    {
        return false;
    }
    const size_t header_length = static_cast<size_t>(data[12] >> 4) * 4;
    if (header_length < TcpHeaderSize || header_length > length)
    {
        return false;
    }
    segment->source_port = ReadBig16(data);
    segment->destination_port = ReadBig16(data + 2);
    segment->sequence = ReadBig32(data + 4);
    segment->acknowledgment = ReadBig32(data + 8);
    segment->flags = data[13] & (TcpFin | TcpSyn | TcpReset | TcpPush | TcpAck);
    segment->window = ReadBig16(data + 14);
    ReadOptions(data + TcpHeaderSize, header_length - TcpHeaderSize, segment);
    segment->payload = data + header_length;
    segment->payload_length = length - header_length;
    return true;
}

size_t WriteTcpSegment(uint8_t* data, size_t capacity, const TcpSegment& segment)
{
    const size_t header_length = TcpHeaderSize + (segment.maximum_segment_size != 0 ? 4 : 0) +
                                 (segment.has_window_scale ? 4 : 0);
    if (data == nullptr || capacity < header_length ||
        segment.payload_length > capacity - header_length)
    {
        return 0;
    }
    WriteBig16(data, segment.source_port);
    WriteBig16(data + 2, segment.destination_port);
    WriteBig32(data + 4, segment.sequence);
    WriteBig32(data + 8, segment.acknowledgment);
    data[12] = static_cast<uint8_t>((header_length / 4) << 4);
    data[13] = segment.flags;
    WriteBig16(data + 14, segment.window);
    WriteBig16(data + TcpChecksumOffset, 0);
    WriteBig16(data + 18, 0);
    uint8_t* option = data + TcpHeaderSize;
    if (segment.maximum_segment_size != 0)
    {
        option[0] = OptionMaximumSegmentSize;
        option[1] = 4;
        WriteBig16(option + 2, segment.maximum_segment_size);
        option += 4;
    }
    if (segment.has_window_scale)
    {
        option[0] = OptionNoOperation;
        option[1] = OptionWindowScale;
        option[2] = 3;
        option[3] = segment.window_scale;
    }
    for (size_t index = 0; index < segment.payload_length; ++index)
    {
        data[header_length + index] = segment.payload[index];
    }
    return header_length + segment.payload_length;
}

uint32_t SequenceLength(const TcpSegment& segment)
{
    return static_cast<uint32_t>(segment.payload_length) + ((segment.flags & TcpSyn) != 0 ? 1 : 0) +
           ((segment.flags & TcpFin) != 0 ? 1 : 0);
}

} // namespace FeatherOS::Network
