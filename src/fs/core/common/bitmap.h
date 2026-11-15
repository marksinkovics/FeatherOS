// SPDX-License-Identifier: MIT OR Apache-2.0
#ifndef FEATHERDISK_COMMON_BITMAP_H
#define FEATHERDISK_COMMON_BITMAP_H

#include <cstdint>
#include <optional>
#include <span>

namespace FeatherDisk
{

// Allocation bitmaps as filesystems store them: bit n is bit n % 8 of byte n / 8.
bool TestBit(std::span<const std::uint8_t> bitmap, std::uint32_t bit);
void SetBit(std::span<std::uint8_t> bitmap, std::uint32_t bit);
void ClearBit(std::span<std::uint8_t> bitmap, std::uint32_t bit);
// The first clear bit in [start, end), where end is at most the bitmap's size in bits.
std::optional<std::uint32_t> FindClearBit(std::span<const std::uint8_t> bitmap, std::uint32_t start,
                                          std::uint32_t end);

} // namespace FeatherDisk

#endif
