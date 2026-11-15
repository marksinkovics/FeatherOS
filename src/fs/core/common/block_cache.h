// SPDX-License-Identifier: MIT OR Apache-2.0
#ifndef FEATHERDISK_COMMON_BLOCK_CACHE_H
#define FEATHERDISK_COMMON_BLOCK_CACHE_H

#include "fs/block_device.h"
#include "fs/error.h"
#include "fs/host.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace FeatherDisk
{

// A few blocks of a volume kept in memory, the least recently used one replaced first, for
// metadata that is read again and again. Callers get copies: a host lock covers only the copying,
// never a device call, so operations may run at the same time.
class BlockCache
{
public:
    static constexpr std::size_t MaximumSlots = 32;

    constexpr BlockCache() = default;
    ~BlockCache();
    BlockCache(const BlockCache&) = delete;
    BlockCache& operator=(const BlockCache&) = delete;

    // block_size is a whole number of the device's sectors; slot_count is clamped to
    // 1..MaximumSlots.
    Status Initialize(BlockDevice& volume, std::uint32_t block_size, std::size_t slot_count);
    std::uint32_t BlockSize() const;

    // Copies out.size() bytes of block, from offset. On a miss the whole block is read into
    // scratch, which is one block long and may be out itself when out is the whole block.
    Status Read(std::uint64_t block, std::size_t offset, std::span<std::uint8_t> out,
                std::span<std::uint8_t> scratch);
    // Whole blocks from the device, bypassing the cache, for file contents.
    Status ReadUncached(std::uint64_t first_block, std::span<std::uint8_t> out);
    // Whole blocks straight to the device (write-through); cached copies of them take the new
    // bytes, and a failed write drops them, since the device may hold part of it.
    Status Write(std::uint64_t first_block, std::span<const std::uint8_t> bytes);

private:
    struct Slot
    {
        std::uint64_t block;
        std::uint64_t last_use;
        bool valid;
    };

    // Under the lock.
    Slot* Find(std::uint64_t block);
    Slot& Victim();
    std::uint8_t* SlotData(const Slot& slot);

    BlockDevice* device = nullptr;
    std::uint32_t block_size = 0;
    std::uint32_t sectors_per_block = 0;
    std::size_t slot_count = 0;
    std::uint8_t* data = nullptr;
    Host::Lock* lock = nullptr;
    std::uint64_t clock = 0;
    std::array<Slot, MaximumSlots> slots{};
};

} // namespace FeatherDisk

#endif
