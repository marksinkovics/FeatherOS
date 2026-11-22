// SPDX-License-Identifier: MIT OR Apache-2.0
#include "common/bitmap.h"

namespace FeatherDisk
{

bool TestBit(std::span<const std::uint8_t> bitmap, std::uint32_t bit)
{
    return (bitmap[bit / 8] & (1U << (bit % 8))) != 0;
}

void SetBit(std::span<std::uint8_t> bitmap, std::uint32_t bit)
{
    bitmap[bit / 8] = static_cast<std::uint8_t>(bitmap[bit / 8] | (1U << (bit % 8)));
}

void ClearBit(std::span<std::uint8_t> bitmap, std::uint32_t bit)
{
    bitmap[bit / 8] = static_cast<std::uint8_t>(bitmap[bit / 8] & ~(1U << (bit % 8)));
}

std::optional<std::uint32_t> FindClearBit(std::span<const std::uint8_t> bitmap, std::uint32_t start,
                                          std::uint32_t end)
{
    const std::uint64_t bits = std::uint64_t{bitmap.size()} * 8;
    if (end > bits)
    {
        end = static_cast<std::uint32_t>(bits);
    }
    std::uint32_t bit = start;
    while (bit < end)
    {
        // Whole bytes that are full are skipped at once.
        if (bit % 8 == 0 && end - bit >= 8 && bitmap[bit / 8] == 0xFF)
        {
            bit += 8;
            continue;
        }
        if (!TestBit(bitmap, bit))
        {
            return bit;
        }
        ++bit;
    }
    return std::nullopt;
}

} // namespace FeatherDisk
