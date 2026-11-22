// SPDX-License-Identifier: MIT OR Apache-2.0
#include "fs/partition.h"

namespace FeatherDisk::Partition
{

namespace
{

constexpr std::uint32_t MinimumSectorSize = 512;

} // namespace

Volume::Volume(BlockDevice& whole_device, Range partition) : device(&whole_device), range(partition)
{
}

void Volume::Assign(BlockDevice& whole_device, Range partition)
{
    device = &whole_device;
    range = partition;
}

std::uint32_t Volume::SectorSize() const
{
    return device == nullptr ? MinimumSectorSize : device->SectorSize();
}

std::uint64_t Volume::SectorCount() const
{
    return device == nullptr ? 0 : range.sector_count;
}

bool Volume::Covers(std::uint64_t first_sector, std::size_t byte_count) const
{
    const std::uint32_t sector_size = SectorSize();
    return device != nullptr && byte_count % sector_size == 0 &&
           first_sector <= range.sector_count &&
           byte_count / sector_size <= range.sector_count - first_sector;
}

Status Volume::Read(std::uint64_t first_sector, std::span<std::uint8_t> buffer)
{
    if (!Covers(first_sector, buffer.size()))
    {
        return std::unexpected(Error::InvalidArgument);
    }
    return device->Read(range.first_sector + first_sector, buffer);
}

Status Volume::Write(std::uint64_t first_sector, std::span<const std::uint8_t> data)
{
    if (!Covers(first_sector, data.size()))
    {
        return std::unexpected(Error::InvalidArgument);
    }
    return device->Write(range.first_sector + first_sector, data);
}

Status Volume::Flush()
{
    if (device == nullptr)
    {
        return std::unexpected(Error::InvalidArgument);
    }
    return device->Flush();
}

} // namespace FeatherDisk::Partition
