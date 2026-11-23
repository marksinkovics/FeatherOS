#include "model_disk.h"

#include <algorithm>

namespace FeatherOS::Storage::Testing
{

ModelDisk::ModelDisk(uint32_t sector_size, uint64_t sectors, bool volatile_cache)
    : sector_size_(sector_size), sectors_(sectors), volatile_cache_(volatile_cache),
      medium_(static_cast<size_t>(sector_size * sectors), 0)
{
}

bool ModelDisk::InRange(uint64_t lba, size_t bytes) const
{
    if (bytes % sector_size_ != 0)
    {
        return false;
    }
    const uint64_t count = bytes / sector_size_;
    return lba <= sectors_ && count <= sectors_ - lba;
}

bool ModelDisk::Write(uint64_t lba, std::span<const uint8_t> data)
{
    if (!InRange(lba, data.size()))
    {
        return false;
    }
    ++write_commands_;
    for (size_t sector = 0; sector < data.size() / sector_size_; ++sector)
    {
        const auto first = data.begin() + static_cast<std::ptrdiff_t>(sector * sector_size_);
        const std::span<const uint8_t> bytes(&*first, sector_size_);
        if (volatile_cache_)
        {
            cache_[lba + sector].assign(bytes.begin(), bytes.end());
        }
        else
        {
            std::copy(bytes.begin(), bytes.end(),
                      medium_.begin() + static_cast<std::ptrdiff_t>((lba + sector) * sector_size_));
        }
    }
    return true;
}

bool ModelDisk::Read(uint64_t lba, std::span<uint8_t> data) const
{
    if (!InRange(lba, data.size()))
    {
        return false;
    }
    for (size_t sector = 0; sector < data.size() / sector_size_; ++sector)
    {
        const auto cached = cache_.find(lba + sector);
        const uint8_t* source =
            cached != cache_.end() ? cached->second.data()
                                   : medium_.data() + (lba + sector) * sector_size_;
        std::copy(source, source + sector_size_, data.begin() + sector * sector_size_);
    }
    return true;
}

void ModelDisk::Flush()
{
    ++flush_commands_;
    for (const auto& [sector, bytes] : cache_)
    {
        std::copy(bytes.begin(), bytes.end(),
                  medium_.begin() + static_cast<std::ptrdiff_t>(sector * sector_size_));
    }
    cache_.clear();
}

void ModelDisk::PowerLoss()
{
    cache_.clear();
}

std::vector<uint8_t> ModelDisk::Durable(uint64_t lba, uint64_t count) const
{
    const auto first = medium_.begin() + static_cast<std::ptrdiff_t>(lba * sector_size_);
    return {first, first + static_cast<std::ptrdiff_t>(count * sector_size_)};
}

uint32_t ModelDisk::SectorSize() const
{
    return sector_size_;
}

uint64_t ModelDisk::Sectors() const
{
    return sectors_;
}

uint32_t ModelDisk::WriteCommands() const
{
    return write_commands_;
}

uint32_t ModelDisk::FlushCommands() const
{
    return flush_commands_;
}

} // namespace FeatherOS::Storage::Testing
