// SPDX-License-Identifier: MIT OR Apache-2.0
#include "common/block_cache.h"

#include "common/bytes.h"

namespace FeatherDisk
{

BlockCache::~BlockCache()
{
    if (lock != nullptr)
    {
        Host::DestroyLock(lock);
    }
    Host::Free(data);
}

Status BlockCache::Initialize(BlockDevice& volume, std::uint32_t size, std::size_t count)
{
    const std::uint32_t sector_size = volume.SectorSize();
    if (device != nullptr || data != nullptr || sector_size == 0 || size < sector_size ||
        size % sector_size != 0)
    {
        return std::unexpected(Error::InvalidArgument);
    }
    if (count > 0)
    {
        count = count > MaximumSlots ? MaximumSlots : count;
        data = static_cast<std::uint8_t*>(Host::Allocate(count * size));
        lock = Host::CreateLock();
        if (data == nullptr || lock == nullptr)
        {
            return std::unexpected(Error::OutOfMemory);
        }
    }
    device = &volume;
    block_size = size;
    sectors_per_block = size / sector_size;
    slot_count = count;
    return {};
}

std::uint32_t BlockCache::BlockSize() const
{
    return block_size;
}

std::size_t BlockCache::SlotCount() const
{
    return slot_count;
}

BlockCache::Slot* BlockCache::Find(std::uint64_t block)
{
    for (std::size_t index = 0; index < slot_count; ++index)
    {
        if (slots[index].valid && slots[index].block == block)
        {
            return &slots[index];
        }
    }
    return nullptr;
}

BlockCache::Slot& BlockCache::Victim()
{
    Slot* oldest = &slots[0];
    for (std::size_t index = 0; index < slot_count; ++index)
    {
        if (!slots[index].valid)
        {
            return slots[index];
        }
        if (slots[index].last_use < oldest->last_use)
        {
            oldest = &slots[index];
        }
    }
    return *oldest;
}

std::uint8_t* BlockCache::SlotData(const Slot& slot)
{
    return data + static_cast<std::size_t>(&slot - slots.data()) * block_size;
}

Status BlockCache::Read(std::uint64_t block, std::size_t offset, std::span<std::uint8_t> out,
                        std::span<std::uint8_t> scratch)
{
    if (device == nullptr || scratch.size() != block_size || offset > block_size ||
        out.size() > block_size - offset)
    {
        return std::unexpected(Error::InvalidArgument);
    }
    if (slot_count == 0)
    {
        if (const Status read = device->Read(block * sectors_per_block, scratch); !read)
        {
            return read;
        }
        if (out.data() != scratch.data() + offset)
        {
            CopyBytes(out.data(), scratch.data() + offset, out.size());
        }
        return {};
    }
    Host::Acquire(*lock);
    if (Slot* slot = Find(block); slot != nullptr)
    {
        slot->last_use = ++clock;
        CopyBytes(out.data(), SlotData(*slot) + offset, out.size());
        Host::Release(*lock);
        return {};
    }
    Host::Release(*lock);

    if (const Status read = device->Read(block * sectors_per_block, scratch); !read)
    {
        return read;
    }
    if (out.data() != scratch.data() + offset)
    {
        CopyBytes(out.data(), scratch.data() + offset, out.size());
    }
    // Another caller may have brought the block in while this one read it.
    Host::Acquire(*lock);
    if (Find(block) == nullptr)
    {
        Slot& slot = Victim();
        slot = {.block = block, .last_use = ++clock, .valid = true};
        CopyBytes(SlotData(slot), scratch.data(), block_size);
    }
    Host::Release(*lock);
    return {};
}

Status BlockCache::ReadUncached(std::uint64_t first_block, std::span<std::uint8_t> out)
{
    if (device == nullptr || out.size() % block_size != 0)
    {
        return std::unexpected(Error::InvalidArgument);
    }
    return device->Read(first_block * sectors_per_block, out);
}

Status BlockCache::Write(std::uint64_t first_block, std::span<const std::uint8_t> bytes)
{
    if (device == nullptr || bytes.size() % block_size != 0)
    {
        return std::unexpected(Error::InvalidArgument);
    }
    const Status written = device->Write(first_block * sectors_per_block, bytes);
    if (slot_count == 0)
    {
        return written;
    }
    const std::uint64_t count = bytes.size() / block_size;
    Host::Acquire(*lock);
    for (std::size_t index = 0; index < slot_count; ++index)
    {
        Slot& slot = slots[index];
        if (!slot.valid || slot.block < first_block || slot.block - first_block >= count)
        {
            continue;
        }
        if (written)
        {
            CopyBytes(SlotData(slot), bytes.data() + (slot.block - first_block) * block_size,
                      block_size);
        }
        else
        {
            slot.valid = false;
        }
    }
    Host::Release(*lock);
    return written;
}

} // namespace FeatherDisk
