// SPDX-License-Identifier: MIT OR Apache-2.0
#include "tests/memory_device.h"

#include <algorithm>
#include <cstring>

namespace FeatherDisk::Tests
{

MemoryDevice::MemoryDevice(std::uint32_t sector_size_in_bytes, std::uint64_t sector_count)
    : sector_size(sector_size_in_bytes), failing_sector(sector_count),
      size(static_cast<std::size_t>(sector_size_in_bytes * sector_count)),
      bytes(std::make_unique<std::uint8_t[]>(size))
{
}

std::uint32_t MemoryDevice::SectorSize() const
{
    return sector_size;
}

std::uint64_t MemoryDevice::SectorCount() const
{
    return size / sector_size;
}

bool MemoryDevice::Covers(std::uint64_t first_sector, std::size_t byte_count) const
{
    return byte_count % sector_size == 0 && first_sector <= SectorCount() &&
           byte_count / sector_size <= SectorCount() - first_sector;
}

Status MemoryDevice::Read(std::uint64_t first_sector, std::span<std::uint8_t> buffer)
{
    if (!Covers(first_sector, buffer.size()))
    {
        return std::unexpected(Error::InvalidArgument);
    }
    if (first_sector + buffer.size() / sector_size > failing_sector ||
        (failing_single >= first_sector &&
         failing_single - first_sector < buffer.size() / sector_size))
    {
        return std::unexpected(Error::Io);
    }
    std::memcpy(buffer.data(), bytes.get() + first_sector * sector_size, buffer.size());
    return {};
}

Status MemoryDevice::Write(std::uint64_t first_sector, std::span<const std::uint8_t> data)
{
    if (!Covers(first_sector, data.size()))
    {
        return std::unexpected(Error::InvalidArgument);
    }
    if (read_only)
    {
        return std::unexpected(Error::ReadOnly);
    }
    if (write_count >= write_limit)
    {
        return std::unexpected(Error::Io);
    }
    ++write_count;
    std::uint8_t* const place = bytes.get() + first_sector * sector_size;
    if (tracking)
    {
        unflushed.push_back({static_cast<std::size_t>(first_sector * sector_size),
                             std::vector<std::uint8_t>(place, place + data.size()),
                             std::vector<std::uint8_t>(data.begin(), data.end())});
    }
    std::memcpy(place, data.data(), data.size());
    return {};
}

Status MemoryDevice::Flush()
{
    ++flush_count;
    flush_writes.push_back(write_count);
    // A device that stopped writing makes nothing durable.
    if (write_count < write_limit)
    {
        unflushed.clear();
    }
    return {};
}

std::span<std::uint8_t> MemoryDevice::Sector(std::uint64_t index)
{
    return Bytes().subspan(index * sector_size, sector_size);
}

std::span<std::uint8_t> MemoryDevice::Bytes()
{
    return {bytes.get(), size};
}

void MemoryDevice::FailReadsFrom(std::uint64_t sector)
{
    failing_sector = std::min(sector, failing_sector);
}

void MemoryDevice::FailReadsAt(std::uint64_t sector)
{
    failing_single = sector;
}

void MemoryDevice::RestoreReads()
{
    failing_single = UINT64_MAX;
}

void MemoryDevice::FailWritesAfter(std::uint64_t count)
{
    write_limit = count;
}

void MemoryDevice::TrackUnflushed()
{
    tracking = true;
}

std::size_t MemoryDevice::UnflushedCount() const
{
    return unflushed.size();
}

std::unique_ptr<MemoryDevice>
MemoryDevice::CopyKeeping(const std::function<bool(std::size_t)>& keep) const
{
    auto copy = std::make_unique<MemoryDevice>(sector_size, size / sector_size);
    std::memcpy(copy->bytes.get(), bytes.get(), size);
    for (auto write = unflushed.rbegin(); write != unflushed.rend(); ++write)
    {
        std::memcpy(copy->bytes.get() + write->offset, write->before.data(), write->before.size());
    }
    for (std::size_t index = 0; index < unflushed.size(); ++index)
    {
        if (keep(index))
        {
            std::memcpy(copy->bytes.get() + unflushed[index].offset, unflushed[index].after.data(),
                        unflushed[index].after.size());
        }
    }
    return copy;
}

} // namespace FeatherDisk::Tests
