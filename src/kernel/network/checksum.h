#ifndef FEATHEROS_NETWORK_CHECKSUM_H
#define FEATHEROS_NETWORK_CHECKSUM_H

#include "address.h"

#include <stddef.h>
#include <stdint.h>

namespace FeatherOS::Network
{

// The Internet checksum (RFC 1071), the ones' complement of the ones' complement sum of 16-bit
// big-endian words. Parts summed separately, such as a pseudo-header and a message, add up to
// the sum of the whole as long as every part but the last has an even length.
uint64_t ChecksumAdd(uint64_t sum, const uint8_t* data, size_t length);
uint16_t ChecksumFinish(uint64_t sum);
// Over data that holds its own checksum field, a correct checksum gives 0.
uint16_t InternetChecksum(const uint8_t* data, size_t length);
// The sum over the pseudo-header that UDP, TCP and ICMPv6 checksums cover besides the message:
// both addresses, the protocol and the message's length, in the layout of the addresses' version.
uint64_t PseudoHeaderSum(const IpAddress& source, const IpAddress& destination, uint8_t protocol,
                         size_t length);

} // namespace FeatherOS::Network

#endif
