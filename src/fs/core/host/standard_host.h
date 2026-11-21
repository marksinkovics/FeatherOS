// SPDX-License-Identifier: MIT OR Apache-2.0
#ifndef FEATHERDISK_STANDARD_HOST_H
#define FEATHERDISK_STANDARD_HOST_H

#include <cstdint>
#include <optional>

namespace FeatherDisk::Host
{

// Makes WallClockNanoseconds return this time until it is called with nullopt, so that two runs
// of the same changes write the same bytes. For tests.
void FreezeWallClock(std::optional<std::uint64_t> nanoseconds);

} // namespace FeatherDisk::Host

#endif
