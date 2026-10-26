#include "heap_allocator.h"

#include <stddef.h>
#include <stdint.h>

namespace FeatherOS::LibC
{

void HeapAllocator::Initialize(Grow grow_function, void* grow_context, Shrink shrink_function)
{
    grow = grow_function;
    shrink = shrink_function;
    context = grow_context;
    free_list = nullptr;
    last_marker = nullptr;
}

void* HeapAllocator::Allocate(size_t size)
{
    const size_t needed = BlockSize(size);
    if (needed == 0)
    {
        return nullptr;
    }
    Block* block = TakeFreeBlock(needed);
    if (block != nullptr)
    {
        return Payload(block);
    }
    // A free block at the end of the heap only needs the difference, if the new memory
    // continues it; otherwise the full size comes in a separate region.
    const size_t tail = TailFreeBytes();
    if (tail > 0 && Extend(needed - tail))
    {
        block = TakeFreeBlock(needed);
    }
    if (block == nullptr && Extend(needed))
    {
        block = TakeFreeBlock(needed);
    }
    return block == nullptr ? nullptr : Payload(block);
}

bool HeapAllocator::Free(void* pointer)
{
    Block* block = BlockOf(pointer);
    if (block == nullptr)
    {
        return false;
    }
    Insert(block);
    ReleaseTail();
    return true;
}

void* HeapAllocator::Resize(void* pointer, size_t size, bool* invalid)
{
    *invalid = false;
    Block* block = BlockOf(pointer);
    if (block == nullptr)
    {
        *invalid = true;
        return nullptr;
    }
    const size_t needed = BlockSize(size);
    if (needed == 0)
    {
        return nullptr;
    }
    if (needed <= block->size)
    {
        Split(block, needed);
        ReleaseTail();
        return pointer;
    }
    // Absorb the free block that follows, if it is adjacent and large enough.
    Block* next = reinterpret_cast<Block*>(reinterpret_cast<uint8_t*>(block) + block->size);
    if (next->magic == FreeMagic && block->size + next->size >= needed && Remove(next))
    {
        next->magic = 0;
        block->size += next->size;
        Split(block, needed);
        return pointer;
    }
    void* moved = Allocate(size);
    if (moved == nullptr)
    {
        return nullptr;
    }
    CopyBytes(moved, pointer, block->size - HeaderSize);
    Insert(block);
    ReleaseTail();
    return moved;
}

size_t HeapAllocator::UsableSize(void* pointer)
{
    const Block* block = BlockOf(pointer);
    return block == nullptr ? 0 : block->size - HeaderSize;
}

size_t HeapAllocator::FreeBytes() const
{
    size_t total = 0;
    for (const Block* block = free_list; block != nullptr; block = block->next)
    {
        total += block->size;
    }
    return total;
}

size_t HeapAllocator::FreeBlockCount() const
{
    size_t count = 0;
    for (const Block* block = free_list; block != nullptr; block = block->next)
    {
        ++count;
    }
    return count;
}

size_t HeapAllocator::BlockSize(size_t size)
{
    if (size > SIZE_MAX / 2)
    {
        return 0;
    }
    const size_t rounded = (size + Alignment - 1) & ~(Alignment - 1);
    const size_t total = rounded + HeaderSize;
    return total < MinimumBlockSize ? MinimumBlockSize : total;
}

void* HeapAllocator::Payload(Block* block)
{
    return reinterpret_cast<uint8_t*>(block) + HeaderSize;
}

void HeapAllocator::CopyBytes(void* destination, const void* source, size_t count)
{
    auto* to = static_cast<uint8_t*>(destination);
    const auto* from = static_cast<const uint8_t*>(source);
    for (size_t index = 0; index < count; ++index)
    {
        to[index] = from[index];
    }
}

HeapAllocator::Block* HeapAllocator::BlockOf(void* pointer)
{
    if (pointer == nullptr || reinterpret_cast<uintptr_t>(pointer) % Alignment != 0)
    {
        return nullptr;
    }
    auto* block = reinterpret_cast<Block*>(static_cast<uint8_t*>(pointer) - HeaderSize);
    return block->magic == UsedMagic ? block : nullptr;
}

void HeapAllocator::Split(Block* block, size_t needed)
{
    if (block->size - needed < MinimumBlockSize)
    {
        return;
    }
    auto* tail = reinterpret_cast<Block*>(reinterpret_cast<uint8_t*>(block) + needed);
    tail->size = block->size - needed;
    tail->magic = UsedMagic;
    block->size = needed;
    Insert(tail);
}

HeapAllocator::Block* HeapAllocator::TakeFreeBlock(size_t needed)
{
    Block** link = &free_list;
    for (Block* block = free_list; block != nullptr; link = &block->next, block = block->next)
    {
        if (block->size < needed)
        {
            continue;
        }
        *link = block->next;
        block->magic = UsedMagic;
        Split(block, needed);
        return block;
    }
    return nullptr;
}

bool HeapAllocator::Remove(Block* target)
{
    for (Block** link = &free_list; *link != nullptr; link = &(*link)->next)
    {
        if (*link == target)
        {
            *link = target->next;
            target->magic = UsedMagic;
            return true;
        }
    }
    return false;
}

bool HeapAllocator::Adjacent(const Block* first, const Block* second)
{
    return reinterpret_cast<const uint8_t*>(first) + first->size ==
           reinterpret_cast<const uint8_t*>(second);
}

void HeapAllocator::Insert(Block* block)
{
    block->magic = FreeMagic;
    Block* previous = nullptr;
    Block* next = free_list;
    while (next != nullptr && next < block)
    {
        previous = next;
        next = next->next;
    }
    block->next = next;
    if (next != nullptr && Adjacent(block, next))
    {
        block->size += next->size;
        block->next = next->next;
        next->magic = 0;
    }
    if (previous != nullptr && Adjacent(previous, block))
    {
        previous->size += block->size;
        previous->next = block->next;
        block->magic = 0;
        return;
    }
    if (previous == nullptr)
    {
        free_list = block;
    }
    else
    {
        previous->next = block;
    }
}

HeapAllocator::Block* HeapAllocator::TailFreeBlock() const
{
    Block* last = free_list;
    while (last != nullptr && last->next != nullptr)
    {
        last = last->next;
    }
    return last != nullptr && last_marker != nullptr && Adjacent(last, last_marker) ? last
                                                                                    : nullptr;
}

size_t HeapAllocator::TailFreeBytes() const
{
    const Block* tail = TailFreeBlock();
    return tail == nullptr ? 0 : tail->size;
}

void HeapAllocator::ReleaseTail()
{
    Block* tail = TailFreeBlock();
    if (shrink == nullptr || tail == nullptr || tail->size < ReleaseThreshold)
    {
        return;
    }
    const size_t release = (tail->size - ReleaseCushion) & ~(GrowthStep - 1);
    uint8_t* end = reinterpret_cast<uint8_t*>(last_marker) + EndMarkerSize;
    if (release == 0 || !shrink(end, release, context))
    {
        return;
    }
    tail->size -= release;
    auto* marker = reinterpret_cast<Block*>(reinterpret_cast<uint8_t*>(tail) + tail->size);
    marker->size = EndMarkerSize;
    marker->magic = UsedMagic;
    last_marker = marker;
}

bool HeapAllocator::Extend(size_t needed)
{
    // The new memory may continue the previous region, whose end marker then becomes part
    // of the free block.
    size_t bytes = needed + EndMarkerSize;
    bytes = bytes < GrowthStep ? GrowthStep : (bytes + GrowthStep - 1) & ~(GrowthStep - 1);
    auto* memory = static_cast<uint8_t*>(grow == nullptr ? nullptr : grow(bytes, context));
    if (memory == nullptr || reinterpret_cast<uintptr_t>(memory) % Alignment != 0)
    {
        return false;
    }
    auto* region = reinterpret_cast<Block*>(memory);
    region->size = bytes - EndMarkerSize;
    auto* marker = reinterpret_cast<Block*>(memory + region->size);
    marker->size = EndMarkerSize;
    marker->magic = UsedMagic;
    if (last_marker != nullptr && reinterpret_cast<uint8_t*>(last_marker) + EndMarkerSize == memory)
    {
        region = last_marker;
        region->size = bytes;
    }
    last_marker = marker;
    Insert(region);
    return true;
}

} // namespace FeatherOS::LibC
