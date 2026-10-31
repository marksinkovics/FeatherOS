#ifndef FEATHEROS_NETWORK_TCP_H
#define FEATHEROS_NETWORK_TCP_H

#include <stddef.h>
#include <stdint.h>

namespace FeatherOS::Network
{

constexpr uint8_t IpProtocolTcp = 6;
constexpr size_t TcpHeaderSize = 20;
constexpr size_t TcpChecksumOffset = 16;
// The options a SYN carries: the maximum segment size, then a no-operation and the window scale.
constexpr size_t TcpMaximumOptionsSize = 8;
// A larger shift in a window scale option counts as this one (RFC 7323 2.3).
constexpr uint8_t TcpMaximumWindowScale = 14;

constexpr uint8_t TcpFin = 0x01;
constexpr uint8_t TcpSyn = 0x02;
constexpr uint8_t TcpReset = 0x04;
constexpr uint8_t TcpPush = 0x08;
constexpr uint8_t TcpAck = 0x10;

struct TcpSegment
{
    uint16_t source_port;
    uint16_t destination_port;
    uint32_t sequence;
    uint32_t acknowledgment;
    uint8_t flags;
    uint16_t window;
    // From the MSS option, which a SYN may carry; 0 without one. Writing a nonzero value adds
    // the option.
    uint16_t maximum_segment_size;
    // From the window scale option, which a SYN may carry. Writing has_window_scale adds the
    // option.
    bool has_window_scale;
    uint8_t window_scale;
    const uint8_t* payload;
    size_t payload_length;
};

// Checks the header against the data and reads the MSS and window scale options, skipping others;
// the checksum is left to the caller, which knows the pseudo-header. The payload points into data.
bool ParseTcpSegment(const uint8_t* data, size_t length, TcpSegment* segment);
// Writes the header and copies the payload after it, with a zero checksum for the IP layer to
// fill in. Returns the segment's length, or 0 when it does not fit in capacity.
size_t WriteTcpSegment(uint8_t* data, size_t capacity, const TcpSegment& segment);
// The sequence space SYN, FIN and the payload take.
uint32_t SequenceLength(const TcpSegment& segment);

// Sequence numbers compare modulo 2^32, so "before" holds across a wrap (RFC 9293 3.4).
constexpr bool SequenceBefore(uint32_t left, uint32_t right)
{
    return static_cast<int32_t>(left - right) < 0;
}

constexpr bool SequenceAtOrBefore(uint32_t left, uint32_t right)
{
    return static_cast<int32_t>(left - right) <= 0;
}

} // namespace FeatherOS::Network

#endif
