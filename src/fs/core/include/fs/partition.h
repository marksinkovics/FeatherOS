// SPDX-License-Identifier: MIT OR Apache-2.0
#ifndef FEATHERDISK_PARTITION_H
#define FEATHERDISK_PARTITION_H

#include "fs/block_device.h"

#include <cstddef>
#include <cstdint>
#include <span>

namespace FeatherDisk::Partition
{

struct Range
{
    std::uint64_t first_sector;
    std::uint64_t sector_count;
    // Linux's partition number: an MBR primary's slot (1 to 4), a logical's place from 5 in chain
    // order, a GPT entry's index plus one. Gaps are normal.
    std::uint32_t number = 0;
};

enum class Scheme : std::uint8_t
{
    None,
    Mbr,
    Gpt,
};

// The table that sector 0 announces: a protective MBR entry means GPT, whether or not the GPT
// itself is intact. A FAT boot sector carries the MBR signature too, so a caller that accepts a
// filesystem on the whole device probes for one first.
Scheme Identify(BlockDevice& device);

// Partitions that may hold a filesystem: the used entries of a GUID partition table, whatever
// their type, when the MBR is protective (numbered by entry; no extended partitions are read),
// otherwise the MBR's FAT16 (0x06, 0x0E) and Linux (0x83) entries. Callers probe each for the
// filesystem they want. A GPT whose header or entry array fails its CRC32 check yields none
// rather than falling back to the MBR.
//
// The MBR's first extended entry (0x05, 0x0F, 0x85) is a container that is never listed; its
// chain of extended boot records is walked after the primaries, which come in slot order. In each
// record entry 0 is a logical partition whose start is relative to that record's sector and
// entry 1 links to the next record, relative to the container's first sector. A logical must lie
// after its record and inside the container and the disk; a link must lie strictly after the
// record that holds it. Anything else, an unreadable record or one without the 0x55AA signature
// ends the chain and keeps what was found. At most 64 records are read. A logical of any type
// takes the next number from 5, listed or not, as in Linux.
//
// Returns how many were stored, at most ranges.size().
std::size_t Find(BlockDevice& device, std::span<Range> ranges);

// One partition of a device as a device of its own, whose sector 0 is the partition's first.
// Without a device it has no sectors.
class Volume final : public BlockDevice
{
public:
    constexpr Volume() = default;
    Volume(BlockDevice& whole_device, Range partition);

    // Only while no filesystem uses the volume.
    void Assign(BlockDevice& whole_device, Range partition);

    std::uint32_t SectorSize() const override;
    std::uint64_t SectorCount() const override;
    Status Read(std::uint64_t first_sector, std::span<std::uint8_t> buffer) override;
    Status Write(std::uint64_t first_sector, std::span<const std::uint8_t> data) override;
    Status Flush() override;

private:
    bool Covers(std::uint64_t first_sector, std::size_t byte_count) const;

    BlockDevice* device = nullptr;
    Range range = {};
};

} // namespace FeatherDisk::Partition

#endif
