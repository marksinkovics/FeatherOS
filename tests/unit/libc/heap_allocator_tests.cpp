#include <catch2/catch_test_macros.hpp>

#include "libc/heap_allocator.h"

#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <vector>

namespace
{

using FeatherOS::LibC::HeapAllocator;

// Stands in for the program break: hands out consecutive memory, optionally leaving a gap
// before each new region so that regions are not contiguous.
struct Arena
{
    static constexpr size_t Capacity = 4 * 1024 * 1024;
    alignas(16) uint8_t bytes[Capacity];
    size_t used = 0;
    size_t limit = Capacity;
    size_t gap = 0;
    int grow_calls = 0;
    int shrink_calls = 0;
    // Set when something else has moved the end, as a program's own sbrk would.
    bool end_moved = false;
};

void* GrowArena(size_t size, void* context)
{
    auto* arena = static_cast<Arena*>(context);
    if (arena->used + arena->gap + size > arena->limit)
    {
        return nullptr;
    }
    arena->used += arena->gap;
    void* memory = arena->bytes + arena->used;
    arena->used += size;
    ++arena->grow_calls;
    return memory;
}

bool ShrinkArena(void* end, size_t size, void* context)
{
    auto* arena = static_cast<Arena*>(context);
    if (arena->end_moved || end != arena->bytes + arena->used || size > arena->used)
    {
        return false;
    }
    arena->used -= size;
    ++arena->shrink_calls;
    return true;
}

struct Heap
{
    explicit Heap(bool shrinking = false) : arena(std::make_unique<Arena>())
    {
        allocator.Initialize(GrowArena, arena.get(), shrinking ? ShrinkArena : nullptr);
    }

    std::unique_ptr<Arena> arena;
    HeapAllocator allocator;
};

bool Aligned(const void* pointer)
{
    return reinterpret_cast<uintptr_t>(pointer) % HeapAllocator::Alignment == 0;
}

} // namespace

TEST_CASE("allocations are aligned, disjoint, and large enough")
{
    Heap heap;
    std::vector<uint8_t*> pointers;
    for (size_t size : {1, 7, 16, 17, 100, 4096, 0})
    {
        auto* pointer = static_cast<uint8_t*>(heap.allocator.Allocate(size));
        REQUIRE(pointer != nullptr);
        REQUIRE(Aligned(pointer));
        REQUIRE(heap.allocator.UsableSize(pointer) >= size);
        std::memset(pointer, static_cast<int>(pointers.size() + 1),
                    heap.allocator.UsableSize(pointer));
        pointers.push_back(pointer);
    }
    for (size_t index = 0; index < pointers.size(); ++index)
    {
        const size_t usable = heap.allocator.UsableSize(pointers[index]);
        for (size_t offset = 0; offset < usable; ++offset)
        {
            REQUIRE(pointers[index][offset] == static_cast<uint8_t>(index + 1));
        }
    }
    // Small allocations come from one growth step.
    REQUIRE(heap.arena->grow_calls == 1);
}

TEST_CASE("freed memory is reused before the heap grows")
{
    Heap heap;
    void* first = heap.allocator.Allocate(200);
    REQUIRE(heap.allocator.Free(first));
    REQUIRE(heap.allocator.Allocate(200) == first);
    for (int round = 0; round < 1000; ++round)
    {
        void* pointer = heap.allocator.Allocate(512);
        REQUIRE(pointer != nullptr);
        REQUIRE(heap.allocator.Free(pointer));
    }
    REQUIRE(heap.arena->grow_calls == 1);
}

TEST_CASE("freeing neighbors in any order merges them back into one block")
{
    Heap heap;
    void* a = heap.allocator.Allocate(1000);
    void* b = heap.allocator.Allocate(1000);
    void* c = heap.allocator.Allocate(1000);
    const size_t free_before = heap.allocator.FreeBytes();
    REQUIRE(heap.allocator.Free(a));
    REQUIRE(heap.allocator.Free(c));
    REQUIRE(heap.allocator.FreeBlockCount() == 2);
    REQUIRE(heap.allocator.Free(b));
    REQUIRE(heap.allocator.FreeBlockCount() == 1);
    REQUIRE(heap.allocator.FreeBytes() == free_before + 3 * (1008 + 16));
    // The merged space holds one allocation as large as all three.
    REQUIRE(heap.allocator.Allocate(3000) == a);
    REQUIRE(heap.arena->grow_calls == 1);
}

TEST_CASE("bad and repeated frees are refused without changing the heap")
{
    Heap heap;
    auto* pointer = static_cast<uint8_t*>(heap.allocator.Allocate(64));
    const size_t free_bytes = heap.allocator.FreeBytes();
    REQUIRE_FALSE(heap.allocator.Free(pointer + 16));
    REQUIRE_FALSE(heap.allocator.Free(pointer + 1));
    REQUIRE(heap.allocator.FreeBytes() == free_bytes);
    REQUIRE(heap.allocator.Free(pointer));
    REQUIRE_FALSE(heap.allocator.Free(pointer));
    bool invalid = false;
    REQUIRE(heap.allocator.Resize(pointer, 10, &invalid) == nullptr);
    REQUIRE(invalid);
    REQUIRE(heap.allocator.UsableSize(pointer) == 0);
}

TEST_CASE("resizing stays in place when it can and otherwise moves the data")
{
    Heap heap;
    bool invalid = true;
    auto* pointer = static_cast<uint8_t*>(heap.allocator.Allocate(100));
    std::memset(pointer, 0x5A, 100);
    REQUIRE(heap.allocator.Resize(pointer, 40, &invalid) == pointer);
    REQUIRE_FALSE(invalid);
    // The block's tail was freed, and growing again takes the free space that follows.
    REQUIRE(heap.allocator.Resize(pointer, 2000, &invalid) == pointer);
    REQUIRE(heap.allocator.UsableSize(pointer) >= 2000);

    void* blocker = heap.allocator.Allocate(16);
    REQUIRE(blocker != nullptr);
    auto* moved = static_cast<uint8_t*>(heap.allocator.Resize(pointer, 8000, &invalid));
    REQUIRE(moved != nullptr);
    REQUIRE(moved != pointer);
    for (size_t index = 0; index < 40; ++index)
    {
        REQUIRE(moved[index] == 0x5A);
    }
    REQUIRE_FALSE(heap.allocator.Free(pointer));
    REQUIRE(heap.allocator.Free(moved));
}

TEST_CASE("the heap grows by as much as a large request needs")
{
    Heap heap;
    void* large = heap.allocator.Allocate(300 * 1024);
    REQUIRE(large != nullptr);
    REQUIRE(heap.allocator.UsableSize(large) >= 300 * 1024);
    REQUIRE(heap.arena->used % HeapAllocator::GrowthStep == 0);
    REQUIRE(heap.allocator.Free(large));
}

TEST_CASE("growth only adds what the free block at the end of the heap lacks")
{
    Heap heap;
    void* small = heap.allocator.Allocate(100);
    REQUIRE(heap.arena->used == HeapAllocator::GrowthStep);
    // Most of the first step is still free at the end, so one more step covers this.
    void* large = heap.allocator.Allocate(HeapAllocator::GrowthStep + 1000);
    REQUIRE(large != nullptr);
    REQUIRE(heap.arena->used == 2 * HeapAllocator::GrowthStep);
    REQUIRE(heap.allocator.Free(small));
    REQUIRE(heap.allocator.Free(large));
    REQUIRE(heap.allocator.FreeBlockCount() == 1);
}

TEST_CASE("contiguous growth merges with the previous region")
{
    Heap heap;
    void* first = heap.allocator.Allocate(HeapAllocator::GrowthStep / 2);
    void* second = heap.allocator.Allocate(HeapAllocator::GrowthStep);
    REQUIRE(heap.arena->grow_calls == 2);
    REQUIRE(heap.allocator.Free(first));
    REQUIRE(heap.allocator.Free(second));
    REQUIRE(heap.allocator.FreeBlockCount() == 1);
    REQUIRE(heap.allocator.Allocate(HeapAllocator::GrowthStep * 3 / 2) == first);
}

TEST_CASE("separate regions stay separate blocks")
{
    Heap heap;
    heap.arena->gap = 4096;
    void* first = heap.allocator.Allocate(HeapAllocator::GrowthStep / 2);
    void* second = heap.allocator.Allocate(HeapAllocator::GrowthStep);
    REQUIRE(heap.allocator.Free(first));
    REQUIRE(heap.allocator.Free(second));
    // The second request first tried to extend the first region's free tail; that memory came
    // as a region of its own, and the full request then came as a third.
    REQUIRE(heap.allocator.FreeBlockCount() == 3);
}

TEST_CASE("allocation fails cleanly when the heap cannot grow")
{
    Heap heap;
    heap.arena->limit = 2 * HeapAllocator::GrowthStep;
    REQUIRE(heap.allocator.Allocate(SIZE_MAX) == nullptr);
    REQUIRE(heap.allocator.Allocate(SIZE_MAX / 2 + 1) == nullptr);
    REQUIRE(heap.allocator.Allocate(3 * HeapAllocator::GrowthStep) == nullptr);
    void* small = heap.allocator.Allocate(100);
    REQUIRE(small != nullptr);
    bool invalid = true;
    REQUIRE(heap.allocator.Resize(small, 3 * HeapAllocator::GrowthStep, &invalid) == nullptr);
    REQUIRE_FALSE(invalid);
    REQUIRE(heap.allocator.UsableSize(small) >= 100);
    REQUIRE(heap.allocator.Free(small));
}

void RunRandomAllocations(Heap& heap, uint32_t maximum_size)
{
    std::map<uint8_t*, std::pair<size_t, uint8_t>> live;
    uint32_t state = 2024;
    auto next = [&state]()
    {
        state = state * 1103515245U + 12345U;
        return state >> 8;
    };
    auto verify = [](uint8_t* pointer, size_t size, uint8_t value)
    {
        size_t index = 0;
        while (index < size && pointer[index] == value)
        {
            ++index;
        }
        REQUIRE(index == size);
    };
    for (int step = 0; step < 5000; ++step)
    {
        const uint32_t action = next() % 3;
        if (action == 0 || live.empty())
        {
            const size_t size = next() % maximum_size;
            auto* pointer = static_cast<uint8_t*>(heap.allocator.Allocate(size));
            REQUIRE(pointer != nullptr);
            const auto value = static_cast<uint8_t>(next());
            std::memset(pointer, value, size);
            live[pointer] = {size, value};
            continue;
        }
        auto entry = live.begin();
        std::advance(entry, static_cast<long>(next() % live.size()));
        uint8_t* pointer = entry->first;
        const auto [size, value] = entry->second;
        verify(pointer, size, value);
        live.erase(entry);
        if (action == 1)
        {
            REQUIRE(heap.allocator.Free(pointer));
            continue;
        }
        const size_t new_size = next() % (maximum_size * 5 / 3);
        bool invalid = true;
        auto* resized = static_cast<uint8_t*>(heap.allocator.Resize(pointer, new_size, &invalid));
        REQUIRE(resized != nullptr);
        verify(resized, size < new_size ? size : new_size, value);
        std::memset(resized, value, new_size);
        live[resized] = {new_size, value};
    }
    for (const auto& [pointer, details] : live)
    {
        verify(pointer, details.first, details.second);
        REQUIRE(heap.allocator.Free(pointer));
    }
    REQUIRE(heap.allocator.FreeBlockCount() == 1);
}

TEST_CASE("random allocations keep their contents until freed")
{
    Heap heap;
    RunRandomAllocations(heap, 3000);
}

TEST_CASE("a large free block at the end of the heap goes back, all but a cushion")
{
    Heap heap(true);
    void* small = heap.allocator.Allocate(100);
    void* large = heap.allocator.Allocate(1024 * 1024);
    REQUIRE(large != nullptr);
    const size_t peak = heap.arena->used;
    REQUIRE(heap.allocator.Free(large));
    REQUIRE(heap.arena->shrink_calls == 1);
    REQUIRE(heap.arena->used < peak);
    REQUIRE(heap.arena->used % HeapAllocator::GrowthStep == 0);
    // What stays free at the end is between the cushion and the threshold.
    REQUIRE(heap.allocator.FreeBytes() >= HeapAllocator::ReleaseCushion);
    REQUIRE(heap.allocator.FreeBytes() < HeapAllocator::ReleaseThreshold);
    // The heap still works, and grows again from its new end.
    void* again = heap.allocator.Allocate(512 * 1024);
    REQUIRE(again != nullptr);
    REQUIRE(heap.allocator.Free(again));
    REQUIRE(heap.allocator.Free(small));
    REQUIRE(heap.allocator.FreeBlockCount() == 1);
}

TEST_CASE("small free blocks and free space below used blocks stay in the heap")
{
    Heap heap(true);
    void* small = heap.allocator.Allocate(100 * 1024);
    REQUIRE(heap.allocator.Free(small));
    REQUIRE(heap.arena->shrink_calls == 0);

    void* first = heap.allocator.Allocate(200 * 1024);
    void* middle = heap.allocator.Allocate(100 * 1024);
    REQUIRE(heap.allocator.Free(first));
    REQUIRE(heap.arena->shrink_calls == 0);
    void* large = heap.allocator.Allocate(400 * 1024);
    REQUIRE(heap.allocator.Free(large));
    REQUIRE(heap.arena->shrink_calls == 1);
    REQUIRE(heap.allocator.FreeBytes() >= 200 * 1024 + HeapAllocator::ReleaseCushion);
    // With middle gone, everything is one free block at the end, which goes back too.
    REQUIRE(heap.allocator.Free(middle));
    REQUIRE(heap.arena->shrink_calls == 2);
    REQUIRE(heap.allocator.FreeBlockCount() == 1);
    REQUIRE(heap.allocator.FreeBytes() < HeapAllocator::ReleaseThreshold);
}

TEST_CASE("the heap keeps its memory when the end has moved")
{
    Heap heap(true);
    void* large = heap.allocator.Allocate(1024 * 1024);
    const size_t peak = heap.arena->used;
    heap.arena->end_moved = true;
    REQUIRE(heap.allocator.Free(large));
    REQUIRE(heap.arena->shrink_calls == 0);
    REQUIRE(heap.arena->used == peak);
    REQUIRE(heap.allocator.Allocate(1024 * 1024) == large);
}

TEST_CASE("random allocations survive the heap shrinking under them")
{
    Heap heap(true);
    RunRandomAllocations(heap, 60000);
    REQUIRE(heap.arena->shrink_calls > 0);
    REQUIRE(heap.arena->used <= HeapAllocator::ReleaseThreshold + HeapAllocator::GrowthStep);
}
