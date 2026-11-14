#include "core_block_device.h"

#include "kernel/storage/block_device.h"

#include <stddef.h>

namespace FeatherOS::BlockDevice
{
namespace
{

class VirtioDisk final : public FeatherDisk::BlockDevice
{
public:
    constexpr VirtioDisk() = default;

    uint32_t SectorSize() const override;
    uint64_t SectorCount() const override;
    FeatherDisk::Status Read(uint64_t first_sector, std::span<uint8_t> buffer) override;
    FeatherDisk::Status Write(uint64_t first_sector, std::span<const uint8_t> data) override;
    FeatherDisk::Status Flush() override;
};

uint32_t VirtioDisk::SectorSize() const
{
    return FeatherOS::BlockDevice::SectorSize;
}

uint64_t VirtioDisk::SectorCount() const
{
    return FeatherOS::BlockDevice::SectorCount();
}

FeatherDisk::Status VirtioDisk::Read(uint64_t first_sector, std::span<uint8_t> buffer)
{
    if (buffer.size() % FeatherOS::BlockDevice::SectorSize != 0)
    {
        return std::unexpected(FeatherDisk::Error::InvalidArgument);
    }
    for (size_t offset = 0; offset < buffer.size(); offset += FeatherOS::BlockDevice::SectorSize)
    {
        if (!ReadSector(first_sector + offset / FeatherOS::BlockDevice::SectorSize,
                        buffer.data() + offset))
        {
            return std::unexpected(FeatherDisk::Error::Io);
        }
    }
    return {};
}

FeatherDisk::Status VirtioDisk::Write(uint64_t first_sector, std::span<const uint8_t> data)
{
    if (data.size() % FeatherOS::BlockDevice::SectorSize != 0)
    {
        return std::unexpected(FeatherDisk::Error::InvalidArgument);
    }
    for (size_t offset = 0; offset < data.size(); offset += FeatherOS::BlockDevice::SectorSize)
    {
        if (!WriteSector(first_sector + offset / FeatherOS::BlockDevice::SectorSize,
                         data.data() + offset))
        {
            return std::unexpected(FeatherDisk::Error::Io);
        }
    }
    return {};
}

FeatherDisk::Status VirtioDisk::Flush()
{
    if (!FeatherOS::BlockDevice::Flush())
    {
        return std::unexpected(FeatherDisk::Error::Io);
    }
    return {};
}

constinit VirtioDisk disk;

} // namespace

FeatherDisk::BlockDevice& CoreDevice()
{
    return disk;
}

} // namespace FeatherOS::BlockDevice
