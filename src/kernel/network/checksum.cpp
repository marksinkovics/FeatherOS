#include "checksum.h"

namespace FeatherOS::Network
{

uint64_t ChecksumAdd(uint64_t sum, const uint8_t* data, size_t length)
{
    size_t index = 0;
    for (; index + 1 < length; index += 2)
    {
        sum += static_cast<uint64_t>(data[index] << 8) | data[index + 1];
    }
    // An odd final byte counts as the high half of a word padded with zero.
    if (index < length)
    {
        sum += static_cast<uint64_t>(data[index]) << 8;
    }
    return sum;
}

uint16_t ChecksumFinish(uint64_t sum)
{
    while ((sum >> 16) != 0)
    {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }
    return static_cast<uint16_t>(~sum);
}

uint16_t InternetChecksum(const uint8_t* data, size_t length)
{
    return ChecksumFinish(ChecksumAdd(0, data, length));
}

uint64_t PseudoHeaderSum(const IpAddress& source, const IpAddress& destination, uint8_t protocol,
                         size_t length)
{
    const size_t address_length = source.version == IpVersion::V6 ? 16 : 4;
    uint64_t sum = ChecksumAdd(0, source.bytes, address_length);
    sum = ChecksumAdd(sum, destination.bytes, address_length);
    return sum + protocol + (length >> 16) + (length & 0xFFFF);
}

} // namespace FeatherOS::Network
