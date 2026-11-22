#include "page_cache_host.h"

#include <catch2/reporters/catch_reporter_event_listener.hpp>
#include <catch2/reporters/catch_reporter_registrars.hpp>

#include <limine.h>

#include "fs/page_cache.h"
#include "kernel/memory/memory_map.h"
#include "kernel/memory/page_allocator.h"
#include "kernel/storage/page_cache.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace
{

namespace Memory = FeatherOS::Memory;
namespace Storage = FeatherOS::BlockDevice;

constexpr std::uint64_t PhysicalBase = 0x100000;
constexpr std::size_t PageSize = 4096;
alignas(4096) std::array<std::uint8_t, HostPageCache::FrameTotal * PageSize> physical_memory;

class PatternDevice final : public FeatherDisk::BlockDevice
{
public:
    std::uint32_t SectorSize() const override
    {
        return 512;
    }

    std::uint64_t SectorCount() const override
    {
        return 100 * 8;
    }

    FeatherDisk::Status Read(std::uint64_t first_sector, std::span<std::uint8_t> buffer) override
    {
        std::memset(buffer.data(), static_cast<int>(first_sector), buffer.size());
        return {};
    }

    FeatherDisk::Status Write(std::uint64_t, std::span<const std::uint8_t>) override
    {
        return {};
    }

    FeatherDisk::Status Flush() override
    {
        return {};
    }
};

// InitializeCache registers the allocator's reclaimer, which exhausting the allocator then uses.
bool CheckReclaimer()
{
    PatternDevice device;
    FeatherDisk::CachedDevice cached;
    cached.Attach(&Storage::Cache(), device);
    std::array<std::uint8_t, PageSize> buffer{};
    for (std::uint64_t page = 0; page < 100; ++page)
    {
        if (!cached.Read(page * 8, buffer))
        {
            return false;
        }
    }
    bool filled = Storage::Cache().Statistics().pages == 100;
    std::vector<std::uint64_t> taken;
    std::uint64_t frame;
    while (Memory::AllocatePageFrame(&frame))
    {
        taken.push_back(frame);
    }
    const bool emptied = Storage::Cache().Statistics().pages == 0;
    for (const std::uint64_t held : taken)
    {
        Memory::FreePageFrame(held);
    }
    cached.Detach();
    return filled && emptied;
}

class CacheListener : public Catch::EventListenerBase
{
public:
    using Catch::EventListenerBase::EventListenerBase;

    void testRunStarting(const Catch::TestRunInfo&) override
    {
        if (!HostPageCache::EnsureCache())
        {
            std::fputs("the host page cache could not be set up\n", stderr);
            std::abort();
        }
    }
};

} // namespace

CATCH_REGISTER_LISTENER(CacheListener)

namespace HostPageCache
{

std::uint64_t HostOffset()
{
    return reinterpret_cast<std::uintptr_t>(physical_memory.data()) - PhysicalBase;
}

bool EnsureAllocator()
{
    static limine_memmap_entry entry{};
    entry.base = PhysicalBase;
    entry.length = FrameTotal * PageSize;
    entry.type = LIMINE_MEMMAP_USABLE;
    static limine_memmap_entry* entries[] = {&entry};
    static limine_memmap_response response{};
    response.entry_count = 1;
    response.entries = entries;
    if (!Memory::InitializeMemoryMapFromResponse(&response))
    {
        return false;
    }
    static bool initialized = false;
    if (!initialized)
    {
        initialized = Memory::InitializePageAllocator(HostOffset());
    }
    return initialized;
}

bool EnsureCache()
{
    static int state = 0;
    if (state == 0)
    {
        state = 2;
        if (EnsureAllocator() && Storage::InitializeCache(HostOffset()) && CheckReclaimer())
        {
            state = 1;
        }
    }
    return state == 1;
}

void InvalidateAll()
{
    if (Storage::CacheReady())
    {
        Storage::Cache().InvalidateAll();
    }
}

} // namespace HostPageCache
