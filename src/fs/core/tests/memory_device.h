// SPDX-License-Identifier: MIT OR Apache-2.0
#ifndef FEATHERDISK_TESTS_MEMORY_DEVICE_H
#define FEATHERDISK_TESTS_MEMORY_DEVICE_H

#include "fs/block_device.h"

#include <cstdint>
#include <span>
#include <vector>

namespace FeatherDisk::Tests
{

class MemoryDevice final : public BlockDevice
{
public:
    MemoryDevice(std::uint32_t sector_size_in_bytes, std::uint64_t sector_count);

    std::uint32_t SectorSize() const override;
    std::uint64_t SectorCount() const override;
    Status Read(std::uint64_t first_sector, std::span<std::uint8_t> buffer) override;
    Status Write(std::uint64_t first_sector, std::span<const std::uint8_t> data) override;
    Status Flush() override;

    std::span<std::uint8_t> Sector(std::uint64_t index);
    // Reads of this sector and later ones fail with Error::Io.
    void FailReadsFrom(std::uint64_t sector);

    bool read_only = false;
    std::uint32_t flush_count = 0;

private:
    bool Covers(std::uint64_t first_sector, std::size_t byte_count) const;

    std::uint32_t sector_size;
    std::uint64_t failing_sector;
    std::vector<std::uint8_t> bytes;
};

} // namespace FeatherDisk::Tests

#endif
