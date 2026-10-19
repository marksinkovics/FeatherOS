#include "heap.h"

#include "kernel/halt.h"
#include "kernel/memory/page_allocator.h"
#include "kernel/memory/virtual_memory.h"
#include "kernel/panic.h"

#include <stdint.h>

namespace
{

constexpr uintptr_t HeapBase = 0xFFFFFD0000000000;
constexpr size_t HeapAlignment = 16;
constexpr uint64_t PageSize = FeatherOS::Memory::PageSize;

struct Block
{
    size_t size;
    Block* previous;
    Block* next;
    bool free;
};

constexpr size_t HeaderSize = (sizeof(Block) + HeapAlignment - 1) & ~(HeapAlignment - 1);

Block* first_block = nullptr;
Block* last_block = nullptr;
uint64_t mapped_page_count = 0;
bool initialized = false;

bool AlignSize(size_t size, size_t* aligned)
{
    if (size == 0)
    {
        size = 1;
    }
    if (size > SIZE_MAX - (HeapAlignment - 1))
    {
        return false;
    }
    *aligned = (size + HeapAlignment - 1) & ~(HeapAlignment - 1);
    return true;
}

uint8_t* Payload(Block* block)
{
    return reinterpret_cast<uint8_t*>(block) + HeaderSize;
}

Block* FindBlock(void* pointer)
{
    if (pointer == nullptr || first_block == nullptr)
    {
        return nullptr;
    }
    for (Block* block = first_block; block != nullptr; block = block->next)
    {
        if (Payload(block) == pointer)
        {
            return block;
        }
    }
    return nullptr;
}

void SplitBlock(Block* block, size_t requested_size)
{
    const size_t remaining = block->size - requested_size;
    if (remaining < HeaderSize + HeapAlignment)
    {
        return;
    }

    auto* split = reinterpret_cast<Block*>(Payload(block) + requested_size);
    split->size = remaining - HeaderSize;
    split->previous = block;
    split->next = block->next;
    split->free = true;
    if (split->next != nullptr)
    {
        split->next->previous = split;
    }
    else
    {
        last_block = split;
    }
    block->next = split;
    block->size = requested_size;
}

bool GrowHeap(size_t minimum_size)
{
    if (minimum_size > SIZE_MAX - HeaderSize)
    {
        return false;
    }
    const uint64_t required_bytes = static_cast<uint64_t>(minimum_size) + HeaderSize;
    if (required_bytes > UINT64_MAX - (PageSize - 1))
    {
        return false;
    }
    const uint64_t pages_needed = (required_bytes + PageSize - 1) / PageSize;
    if (pages_needed == 0 || mapped_page_count > (UINTPTR_MAX - HeapBase) / PageSize ||
        pages_needed > (UINTPTR_MAX - HeapBase) / PageSize - mapped_page_count)
    {
        return false;
    }

    const uint64_t first_new_page = mapped_page_count;
    uint64_t mapped_pages = 0;
    while (mapped_pages < pages_needed)
    {
        uint64_t physical_address;
        if (!FeatherOS::Memory::AllocatePageFrame(&physical_address))
        {
            break;
        }
        const uintptr_t virtual_address =
            HeapBase + static_cast<uintptr_t>(mapped_page_count * PageSize);
        if (!FeatherOS::Memory::MapPage(virtual_address, physical_address,
                                        FeatherOS::Memory::PageFlags::Read |
                                            FeatherOS::Memory::PageFlags::Write))
        {
            FeatherOS::Memory::FreePageFrame(physical_address);
            break;
        }
        ++mapped_page_count;
        ++mapped_pages;
    }
    if (mapped_pages == 0)
    {
        return false;
    }

    const uintptr_t region_start = HeapBase + static_cast<uintptr_t>(first_new_page * PageSize);
    const size_t region_size = static_cast<size_t>(mapped_pages * PageSize);
    if (last_block != nullptr && last_block->free &&
        Payload(last_block) + last_block->size == reinterpret_cast<uint8_t*>(region_start))
    {
        last_block->size += region_size;
    }
    else
    {
        if (region_size <= HeaderSize)
        {
            return false;
        }
        auto* block = reinterpret_cast<Block*>(region_start);
        block->size = region_size - HeaderSize;
        block->previous = last_block;
        block->next = nullptr;
        block->free = true;
        if (last_block == nullptr)
        {
            first_block = block;
        }
        else
        {
            last_block->next = block;
        }
        last_block = block;
    }
    return true;
}

} // namespace

namespace FeatherOS::Memory
{

bool InitializeHeap()
{
    if (initialized)
    {
        return false;
    }
    initialized = true;
    return true;
}

void* Allocate(size_t size)
{
    size_t aligned_size;
    if (!initialized || !AlignSize(size, &aligned_size))
    {
        return nullptr;
    }

    for (;;)
    {
        for (Block* block = first_block; block != nullptr; block = block->next)
        {
            if (block->free && block->size >= aligned_size)
            {
                SplitBlock(block, aligned_size);
                block->free = false;
                return Payload(block);
            }
        }
        if (!GrowHeap(aligned_size))
        {
            return nullptr;
        }
    }
}

void* AllocateZeroed(size_t count, size_t size)
{
    if (size != 0 && count > SIZE_MAX / size)
    {
        return nullptr;
    }
    const size_t total_size = count * size;
    auto* memory = static_cast<uint8_t*>(Allocate(total_size));
    if (memory == nullptr)
    {
        return nullptr;
    }
    for (size_t index = 0; index < total_size; ++index)
    {
        memory[index] = 0;
    }
    return memory;
}

bool Deallocate(void* pointer)
{
    if (pointer == nullptr)
    {
        return true;
    }
    Block* block = FindBlock(pointer);
    if (block == nullptr || block->free)
    {
        return false;
    }
    block->free = true;

    if (block->previous != nullptr && block->previous->free)
    {
        Block* previous = block->previous;
        previous->size += HeaderSize + block->size;
        previous->next = block->next;
        if (block->next != nullptr)
        {
            block->next->previous = previous;
        }
        else
        {
            last_block = previous;
        }
        block = previous;
    }
    if (block->next != nullptr && block->next->free)
    {
        Block* next = block->next;
        block->size += HeaderSize + next->size;
        block->next = next->next;
        if (next->next != nullptr)
        {
            next->next->previous = block;
        }
        else
        {
            last_block = block;
        }
    }
    return true;
}

} // namespace FeatherOS::Memory

extern "C" void* malloc(size_t size)
{
    return FeatherOS::Memory::Allocate(size);
}

extern "C" void* calloc(size_t count, size_t size)
{
    return FeatherOS::Memory::AllocateZeroed(count, size);
}

extern "C" void free(void* pointer)
{
    if (!FeatherOS::Memory::Deallocate(pointer))
    {
        FeatherOS::Panic("Invalid or duplicate heap free");
    }
}

void* operator new(size_t size)
{
    void* memory = FeatherOS::Memory::Allocate(size);
    if (memory == nullptr)
    {
        FeatherOS::Panic("Kernel heap exhausted during operator new");
    }
    return memory;
}

void* operator new[](size_t size)
{
    return ::operator new(size);
}

void operator delete(void* pointer) noexcept
{
    if (!FeatherOS::Memory::Deallocate(pointer))
    {
        FeatherOS::Panic("Invalid or duplicate operator delete");
    }
}

void operator delete[](void* pointer) noexcept
{
    ::operator delete(pointer);
}

void operator delete(void* pointer, size_t) noexcept
{
    ::operator delete(pointer);
}

void operator delete[](void* pointer, size_t) noexcept
{
    ::operator delete(pointer);
}