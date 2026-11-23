#ifndef FEATHEROS_TESTS_STORAGE_MODEL_DISK_H
#define FEATHEROS_TESTS_STORAGE_MODEL_DISK_H

#include <cstdint>
#include <map>
#include <span>
#include <vector>

namespace FeatherOS::Storage::Testing
{

// A disk with an optional volatile write cache: a write is visible to reads at once but reaches
// the medium only at Flush, and PowerLoss drops what the cache still holds. Without the cache,
// every write is durable when it returns.
class ModelDisk
{
public:
    ModelDisk(uint32_t sector_size, uint64_t sectors, bool volatile_cache);

    // False, with nothing changed, when the range is outside the disk or not whole sectors.
    bool Write(uint64_t lba, std::span<const uint8_t> data);
    bool Read(uint64_t lba, std::span<uint8_t> data) const;
    void Flush();
    void PowerLoss();

    // What a read would return after a power loss now.
    std::vector<uint8_t> Durable(uint64_t lba, uint64_t count) const;

    uint32_t SectorSize() const;
    uint64_t Sectors() const;
    uint32_t WriteCommands() const;
    uint32_t FlushCommands() const;

private:
    bool InRange(uint64_t lba, size_t bytes) const;

    uint32_t sector_size_;
    uint64_t sectors_;
    bool volatile_cache_;
    std::vector<uint8_t> medium_;
    std::map<uint64_t, std::vector<uint8_t>> cache_;
    uint32_t write_commands_ = 0;
    uint32_t flush_commands_ = 0;
};

} // namespace FeatherOS::Storage::Testing

#endif
