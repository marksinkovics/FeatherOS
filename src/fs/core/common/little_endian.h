// SPDX-License-Identifier: MIT OR Apache-2.0
#ifndef FEATHERDISK_COMMON_LITTLE_ENDIAN_H
#define FEATHERDISK_COMMON_LITTLE_ENDIAN_H

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace FeatherDisk
{

template <typename T>
concept LittleEndianInteger = std::is_unsigned_v<T> && !std::is_same_v<T, bool>;

// Byte by byte, so on-disk fields need no alignment and the host's byte order does not matter.
template <LittleEndianInteger T> constexpr T LoadLittleEndian(const std::uint8_t* bytes)
{
    T value = 0;
    for (std::size_t index = 0; index < sizeof(T); ++index)
    {
        value = static_cast<T>(value | static_cast<T>(static_cast<T>(bytes[index]) << (8 * index)));
    }
    return value;
}

template <LittleEndianInteger T> constexpr void StoreLittleEndian(std::uint8_t* bytes, T value)
{
    for (std::size_t index = 0; index < sizeof(T); ++index)
    {
        bytes[index] = static_cast<std::uint8_t>(value >> (8 * index));
    }
}

} // namespace FeatherDisk

#endif
