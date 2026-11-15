// SPDX-License-Identifier: MIT OR Apache-2.0
#include "common/bytes.h"

namespace FeatherDisk
{

void CopyBytes(void* to, const void* from, std::size_t count)
{
    auto* target = static_cast<std::uint8_t*>(to);
    const auto* source = static_cast<const std::uint8_t*>(from);
    for (std::size_t index = 0; index < count; ++index)
    {
        target[index] = source[index];
    }
}

void ZeroBytes(std::span<std::uint8_t> bytes)
{
    for (std::uint8_t& byte : bytes)
    {
        byte = 0;
    }
}

} // namespace FeatherDisk
