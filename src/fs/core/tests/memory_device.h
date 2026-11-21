// SPDX-License-Identifier: MIT OR Apache-2.0
#ifndef FEATHERDISK_TESTS_MEMORY_DEVICE_H
#define FEATHERDISK_TESTS_MEMORY_DEVICE_H

#include "fs/block_device.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
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
    // The whole device.
    std::span<std::uint8_t> Bytes();
    // Reads of this sector and later ones fail with Error::Io.
    void FailReadsFrom(std::uint64_t sector);
    // Reads that include this sector fail with Error::Io until RestoreReads.
    void FailReadsAt(std::uint64_t sector);
    void RestoreReads();
    // Writes after the first count fail with Error::Io and change nothing, as if the machine
    // stopped there.
    void FailWritesAfter(std::uint64_t count);
    // With tracking on, the writes since the last Flush are remembered, so that a copy can show
    // the medium as if only some of them had reached it, as a drive's volatile cache may leave it
    // after a power cut.
    void TrackUnflushed();
    std::size_t UnflushedCount() const;
    // keep is asked about each unflushed write by its index, oldest first.
    std::unique_ptr<MemoryDevice> CopyKeeping(const std::function<bool(std::size_t)>& keep) const;

    bool read_only = false;
    // Reads that reached the device, failed ones included, and the sectors they asked for.
    std::uint64_t read_count = 0;
    std::uint64_t read_sectors = 0;
    // Called after a successful read has copied its bytes out, with the sector range; a test
    // changes the device here to interleave with a read. It may reset itself.
    std::function<void(std::uint64_t first_sector, std::uint64_t sector_count)> on_read;
    // A hash of every accepted write (its sector and bytes) and every flush, in order, so two runs
    // can be compared for sending the device the same calls.
    std::uint64_t call_digest = 14695981039346656037ULL;
    std::uint32_t flush_count = 0;
    std::uint64_t write_count = 0;
    // write_count at each Flush, which marks where a commit or checkpoint reached the device.
    std::vector<std::uint64_t> flush_writes;

private:
    struct Unflushed
    {
        std::size_t offset;
        std::vector<std::uint8_t> before;
        std::vector<std::uint8_t> after;
    };

    bool Covers(std::uint64_t first_sector, std::size_t byte_count) const;
    void Digest(std::uint64_t tag, std::span<const std::uint8_t> data);

    std::uint32_t sector_size;
    std::uint64_t failing_sector;
    std::uint64_t failing_single = UINT64_MAX;
    std::uint64_t write_limit = UINT64_MAX;
    bool tracking = false;
    std::vector<Unflushed> unflushed;
    // Not a vector: tests copy 8 MiB devices hundreds of times, and an unoptimised vector frees
    // its bytes one by one.
    std::size_t size;
    std::unique_ptr<std::uint8_t[]> bytes;
};

} // namespace FeatherDisk::Tests

#endif
