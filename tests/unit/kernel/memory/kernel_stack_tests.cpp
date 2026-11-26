#include <catch2/catch_test_macros.hpp>

#include "kernel/memory/kernel_stack.h"
#include "kernel/memory/page_allocator.h"
#include "kernel/memory/virtual_memory.h"

#include "support/page_cache_host.h"

#include <cstdint>
#include <map>
#include <set>

namespace
{

namespace Memory = FeatherOS::Memory;
constexpr std::uint64_t PageSize = Memory::PageSize;
constexpr std::uint64_t StackPages = Memory::KernelStackSize / PageSize;
// One guard page precedes each stack.
constexpr std::uint64_t SlotSize = PageSize + Memory::KernelStackSize;
constexpr std::uint64_t MaximumSlots = 65536;

std::map<std::uint64_t, std::uint64_t> mapped_pages;
// The MapPage call that fails, counted from 0; -1 for none.
int failing_map_call = -1;
int map_calls = 0;

} // namespace

namespace FeatherOS::Memory
{

bool MapPage(std::uint64_t virtual_address, std::uint64_t physical_address, PageFlags)
{
    if (map_calls++ == failing_map_call)
    {
        return false;
    }
    mapped_pages[virtual_address] = physical_address;
    return true;
}

bool UnmapPage(std::uint64_t virtual_address)
{
    return mapped_pages.erase(virtual_address) == 1;
}

} // namespace FeatherOS::Memory

namespace
{

// Takes the free frames and gives some back, so a test decides where AllocatePageFrame fails.
class Frames
{
public:
    Frames()
    {
        REQUIRE(HostPageCache::EnsureAllocator());
        Memory::SetFrameReclaimer(nullptr);
        mapped_pages.clear();
        failing_map_call = -1;
        map_calls = 0;
        free_before = Memory::FreeFrameCount();
    }

    ~Frames()
    {
        failing_map_call = -1;
        for (const std::uint64_t frame : held)
        {
            REQUIRE(Memory::FreePageFrame(frame));
        }
        // A stack the test kept leaves its four frames mapped; they are returned here.
        for (const auto& [address, frame] : mapped_pages)
        {
            (void)address;
            REQUIRE(Memory::FreePageFrame(frame));
        }
        mapped_pages.clear();
        CHECK(Memory::FreeFrameCount() == free_before);
    }

    void TakeAll()
    {
        std::uint64_t frame;
        while (Memory::AllocatePageFrame(&frame))
        {
            held.insert(frame);
        }
    }

    void GiveBack(std::uint64_t count)
    {
        for (std::uint64_t index = 0; index < count && !held.empty(); ++index)
        {
            REQUIRE(Memory::FreePageFrame(*held.begin()));
            held.erase(held.begin());
        }
    }

    std::uint64_t free_before = 0;
    std::set<std::uint64_t> held;
};

} // namespace

TEST_CASE("a kernel stack that gets no frame hands its slot back", "[kernel-stack][R3-07]")
{
    Frames frames;
    std::uint8_t* const first = Memory::AllocateKernelStack();
    REQUIRE(first != nullptr);
    frames.TakeAll();
    for (std::uint64_t granted = 0; granted < StackPages; ++granted)
    {
        frames.GiveBack(granted);
        INFO("frames available before the failing request: " << granted);
        CHECK(Memory::AllocateKernelStack() == nullptr);
        CHECK(mapped_pages.size() == StackPages);
        CHECK(Memory::FreeFrameCount() == granted);
        frames.TakeAll();
    }

    frames.GiveBack(StackPages);
    std::uint8_t* const second = Memory::AllocateKernelStack();
    REQUIRE(second != nullptr);
    CHECK(second == first + SlotSize);
}

TEST_CASE("a kernel stack whose mapping fails hands its slot back", "[kernel-stack][R3-07]")
{
    Frames frames;
    std::uint8_t* const first = Memory::AllocateKernelStack();
    REQUIRE(first != nullptr);
    const std::uint64_t free_with_first = Memory::FreeFrameCount();

    for (int failing = 0; failing < static_cast<int>(StackPages); ++failing)
    {
        INFO("failing MapPage call: " << failing);
        map_calls = 0;
        failing_map_call = failing;
        CHECK(Memory::AllocateKernelStack() == nullptr);
        CHECK(mapped_pages.size() == StackPages);
        CHECK(Memory::FreeFrameCount() == free_with_first);
    }

    failing_map_call = -1;
    std::uint8_t* const second = Memory::AllocateKernelStack();
    REQUIRE(second != nullptr);
    CHECK(second == first + SlotSize);
}

TEST_CASE("more failed kernel stacks than slots leave the pool usable", "[kernel-stack][R3-07]")
{
    Frames frames;
    frames.TakeAll();
    for (std::uint64_t attempt = 0; attempt < MaximumSlots + 16; ++attempt)
    {
        REQUIRE(Memory::AllocateKernelStack() == nullptr);
    }

    frames.GiveBack(StackPages);
    CHECK(Memory::AllocateKernelStack() != nullptr);
}

TEST_CASE("slots returned by failures are reused before new ones", "[kernel-stack][R3-07]")
{
    Frames frames;
    std::uint8_t* const first = Memory::AllocateKernelStack();
    REQUIRE(first != nullptr);

    // Two stacks fail while a third succeeds in between, so the returned slots are not the last.
    failing_map_call = 0;
    map_calls = 0;
    CHECK(Memory::AllocateKernelStack() == nullptr);
    failing_map_call = -1;
    std::uint8_t* const second = Memory::AllocateKernelStack();
    REQUIRE(second != nullptr);
    CHECK(second == first + SlotSize);
    failing_map_call = map_calls;
    CHECK(Memory::AllocateKernelStack() == nullptr);
    failing_map_call = -1;
    std::uint8_t* const third = Memory::AllocateKernelStack();
    REQUIRE(third != nullptr);
    CHECK(third == second + SlotSize);
}
