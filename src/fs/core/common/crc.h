// SPDX-License-Identifier: MIT OR Apache-2.0
#ifndef FEATHERDISK_COMMON_CRC_H
#define FEATHERDISK_COMMON_CRC_H

#include <cstdint>
#include <span>

namespace FeatherDisk
{

// Both in Linux's form, which on-disk formats store: the caller passes the running value (the
// seed first), and nothing is inverted before or after, so a checksum can continue over several
// pieces. The standard CRC-32C (Castagnoli, reflected) is ~Crc32c(~0U, data), and CRC-16/MODBUS
// (polynomial 0x8005, reflected) is Crc16(0xFFFF, data).
std::uint32_t Crc32c(std::uint32_t crc, std::span<const std::uint8_t> bytes);
std::uint16_t Crc16(std::uint16_t crc, std::span<const std::uint8_t> bytes);

} // namespace FeatherDisk

#endif
