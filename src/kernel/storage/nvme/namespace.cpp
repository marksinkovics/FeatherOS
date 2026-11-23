#include "namespace.h"

#include "controller.h"

namespace FeatherOS::Storage::Nvme
{

void Namespace::Initialize(Controller& controller, const DiskInfo& info)
{
    controller_ = &controller;
    info_ = info;
}

uint32_t Namespace::Id() const
{
    return info_.namespace_id;
}

uint32_t Namespace::SectorSize() const
{
    return info_.sector_size;
}

uint64_t Namespace::SectorCount() const
{
    return info_.sector_count;
}

bool Namespace::ReadOnly() const
{
    return info_.read_only;
}

DiskInfo Namespace::Info()
{
    return info_;
}

bool Namespace::Gone()
{
    return controller_->Dead();
}

FeatherDisk::Status Namespace::Read(uint64_t sector, uint32_t count, uint8_t* buffer)
{
    if (sector > info_.sector_count || count > info_.sector_count - sector)
    {
        return std::unexpected(FeatherDisk::Error::InvalidArgument);
    }
    return controller_->Io(*this, false, sector, count, buffer, nullptr);
}

FeatherDisk::Status Namespace::Write(uint64_t sector, uint32_t count, const uint8_t* buffer)
{
    if (sector > info_.sector_count || count > info_.sector_count - sector)
    {
        return std::unexpected(FeatherDisk::Error::InvalidArgument);
    }
    return controller_->Io(*this, true, sector, count, nullptr, buffer);
}

FeatherDisk::Status Namespace::Flush()
{
    return controller_->FlushNamespace(*this);
}

FeatherDisk::Status Namespace::Stop()
{
    return std::unexpected(FeatherDisk::Error::Unsupported);
}

void Namespace::Release() {}

} // namespace FeatherOS::Storage::Nvme
