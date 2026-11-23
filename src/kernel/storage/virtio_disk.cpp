#include "virtio_disk.h"

#include "kernel/storage/block_device.h"
#include "kernel/storage/disk.h"

#include <expected>
#include <stddef.h>

namespace FeatherOS::BlockDevice
{
namespace
{

constexpr uint32_t MaxTransferBytes = 64 * 1024;

class VirtioDriver final : public Storage::DiskDriver
{
public:
    constexpr VirtioDriver() = default;

    Storage::DiskInfo Info() override;
    bool Gone() override;
    FeatherDisk::Status Read(uint64_t sector, uint32_t count, uint8_t* buffer) override;
    FeatherDisk::Status Write(uint64_t sector, uint32_t count, const uint8_t* buffer) override;
    FeatherDisk::Status Flush() override;
    FeatherDisk::Status Stop() override;
    void Release() override;
};

Storage::DiskInfo VirtioDriver::Info()
{
    Storage::DiskInfo info = {};
    info.kind = Storage::DiskKind::Virtio;
    info.sector_size = SectorSize;
    info.sector_count = SectorCount();
    info.max_transfer_bytes = MaxTransferBytes;
    return info;
}

bool VirtioDriver::Gone()
{
    return false;
}

FeatherDisk::Status VirtioDriver::Read(uint64_t sector, uint32_t count, uint8_t* buffer)
{
    for (uint32_t index = 0; index < count; ++index)
    {
        if (!ReadSector(sector + index, buffer + static_cast<size_t>(index) * SectorSize))
        {
            return std::unexpected(FeatherDisk::Error::Io);
        }
    }
    return {};
}

FeatherDisk::Status VirtioDriver::Write(uint64_t sector, uint32_t count, const uint8_t* buffer)
{
    for (uint32_t index = 0; index < count; ++index)
    {
        if (!WriteSector(sector + index, buffer + static_cast<size_t>(index) * SectorSize))
        {
            return std::unexpected(FeatherDisk::Error::Io);
        }
    }
    return {};
}

FeatherDisk::Status VirtioDriver::Flush()
{
    if (!BlockDevice::Flush())
    {
        return std::unexpected(FeatherDisk::Error::Io);
    }
    return {};
}

FeatherDisk::Status VirtioDriver::Stop()
{
    return std::unexpected(FeatherDisk::Error::Unsupported);
}

void VirtioDriver::Release() {}

constinit VirtioDriver driver;

} // namespace

bool RegisterVirtioDisk()
{
    return Storage::Register(driver) != nullptr;
}

} // namespace FeatherOS::BlockDevice
