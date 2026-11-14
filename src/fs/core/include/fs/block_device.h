// SPDX-License-Identifier: MIT OR Apache-2.0
#ifndef FEATHERDISK_BLOCK_DEVICE_H
#define FEATHERDISK_BLOCK_DEVICE_H

#include "fs/error.h"

#include <cstdint>
#include <span>

namespace FeatherDisk
{

// A disk, or one partition of it, as each host provides it. Buffers hold a whole number of
// sectors, and every sector they cover must lie on the device.
class BlockDevice
{
public:
    BlockDevice() = default;
    BlockDevice(const BlockDevice&) = delete;
    BlockDevice& operator=(const BlockDevice&) = delete;

    // A power of two, at least 512.
    virtual std::uint32_t SectorSize() const = 0;
    virtual std::uint64_t SectorCount() const = 0;
    virtual Status Read(std::uint64_t first_sector, std::span<std::uint8_t> buffer) = 0;
    // A read-only device fails with Error::ReadOnly.
    virtual Status Write(std::uint64_t first_sector, std::span<const std::uint8_t> data) = 0;
    // Makes every completed write durable.
    virtual Status Flush() = 0;

protected:
    // The core never owns a device, so it never deletes one through this interface.
    ~BlockDevice() = default;
};

} // namespace FeatherDisk

#endif
