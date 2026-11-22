// SPDX-License-Identifier: MIT OR Apache-2.0
#ifndef FEATHERDISK_COMMON_BYTES_H
#define FEATHERDISK_COMMON_BYTES_H

#include <cstddef>
#include <cstdint>
#include <span>

namespace FeatherDisk
{

// The core has no <cstring>. The ranges must not overlap.
void CopyBytes(void* to, const void* from, std::size_t count);
void ZeroBytes(std::span<std::uint8_t> bytes);

} // namespace FeatherDisk

#endif
