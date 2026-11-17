// SPDX-License-Identifier: MIT OR Apache-2.0
#include "common/crc.h"

#include <array>
#include <cstddef>

namespace FeatherDisk
{
namespace
{

// The polynomials bit-reversed, as the reflected forms process the low bit first.
constexpr std::uint32_t Crc32cPolynomial = 0x82F63B78;
constexpr std::uint16_t Crc16Polynomial = 0xA001;

template <typename T> constexpr std::array<T, 256> MakeTable(T polynomial)
{
    std::array<T, 256> table{};
    for (std::size_t index = 0; index < table.size(); ++index)
    {
        auto value = static_cast<T>(index);
        for (int bit = 0; bit < 8; ++bit)
        {
            value = static_cast<T>((value & 1U) != 0 ? (value >> 1) ^ polynomial : value >> 1);
        }
        table[index] = value;
    }
    return table;
}

constexpr std::array<std::uint32_t, 256> Crc32cTable = MakeTable(Crc32cPolynomial);
constexpr std::array<std::uint16_t, 256> Crc16Table = MakeTable(Crc16Polynomial);

} // namespace

std::uint32_t Crc32c(std::uint32_t crc, std::span<const std::uint8_t> bytes)
{
    for (const std::uint8_t byte : bytes)
    {
        crc = Crc32cTable[(crc ^ byte) & 0xFFU] ^ (crc >> 8);
    }
    return crc;
}

std::uint16_t Crc16(std::uint16_t crc, std::span<const std::uint8_t> bytes)
{
    for (const std::uint8_t byte : bytes)
    {
        crc = static_cast<std::uint16_t>(Crc16Table[(crc ^ byte) & 0xFFU] ^ (crc >> 8));
    }
    return crc;
}

} // namespace FeatherDisk
