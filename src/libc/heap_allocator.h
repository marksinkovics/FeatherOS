#ifndef FEATHEROS_LIBC_HEAP_ALLOCATOR_H
#define FEATHEROS_LIBC_HEAP_ALLOCATOR_H

#include <stddef.h>
#include <stdint.h>

namespace FeatherOS::LibC
{

// A first-fit allocator over memory obtained from Grow (ExtendHeap in programs). Every block
// starts with a 16-byte header, so payloads keep the 16-byte alignment malloc promises. Free
// blocks form a list sorted by address, which lets free merge a block with free neighbors on
// either side.
class HeapAllocator
{
public:
    // Returns bytes of new memory, or nullptr. Memory from consecutive calls may be contiguous,
    // as a growing program break is.
    using Grow = void* (*)(size_t bytes, void* context);
    // Gives back the last bytes before end, if end is still where the memory from Grow stops
    // (nothing else grew the break since). False if it could not.
    using Shrink = bool (*)(void* end, size_t bytes, void* context);

    static constexpr size_t Alignment = 16;
    // Requests the heap in steps of at least this much, so small allocations rarely need a
    // syscall.
    static constexpr size_t GrowthStep = 64 * 1024;
    // When the free block at the end of the heap reaches this size, it is cut back to
    // ReleaseCushion and the rest is returned, in whole growth steps. The cushion keeps a
    // program that frees and allocates in a loop from making a syscall each time.
    static constexpr size_t ReleaseThreshold = 256 * 1024;
    static constexpr size_t ReleaseCushion = GrowthStep;

    // Must be called first; a global allocator then needs no constructor.
    void Initialize(Grow grow_function, void* grow_context, Shrink shrink_function = nullptr)
    {
        grow = grow_function;
        shrink = shrink_function;
        context = grow_context;
        free_list = nullptr;
        last_marker = nullptr;
    }

    // nullptr if size does not fit, or if Grow could not provide the memory.
    void* Allocate(size_t size)
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

    // False, and nothing changes, for a pointer this allocator did not hand out or that was
    // already freed.
    bool Free(void* pointer)
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

    // Grows or shrinks an allocation in place when it can, and otherwise moves it. Returns the
    // new pointer, or nullptr (leaving the old allocation intact) if there is no memory.
    void* Resize(void* pointer, size_t size, bool* invalid)
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

    // The bytes the caller may use, at least what it asked for; 0 for an invalid pointer.
    size_t UsableSize(void* pointer)
    {
        const Block* block = BlockOf(pointer);
        return block == nullptr ? 0 : block->size - HeaderSize;
    }

    size_t FreeBytes() const
    {
        size_t total = 0;
        for (const Block* block = free_list; block != nullptr; block = block->next)
        {
            total += block->size;
        }
        return total;
    }

    size_t FreeBlockCount() const
    {
        size_t count = 0;
        for (const Block* block = free_list; block != nullptr; block = block->next)
        {
            ++count;
        }
        return count;
    }

private:
    // In a used block only size and magic are in use: next overlaps the first payload bytes.
    struct Block
    {
        size_t size;
        uint64_t magic;
        Block* next;
    };

    static constexpr size_t HeaderSize = 16;
    // A free block must hold its header and the list pointer.
    static constexpr size_t MinimumBlockSize = 32;
    // Distinct values catch frees of pointers that never came from malloc, and double frees.
    static constexpr uint64_t UsedMagic = 0x4645415448455255;
    static constexpr uint64_t FreeMagic = 0x4645415448455246;
    // A header-only block that is never freed ends each region, so the block before it never
    // mistakes memory past the region for a neighbor.
    static constexpr size_t EndMarkerSize = HeaderSize;

    static_assert(sizeof(Block) <= MinimumBlockSize, "a free block must fit its list pointer");

    static size_t BlockSize(size_t size)
    {
        if (size > SIZE_MAX / 2)
        {
            return 0;
        }
        const size_t rounded = (size + Alignment - 1) & ~(Alignment - 1);
        const size_t total = rounded + HeaderSize;
        return total < MinimumBlockSize ? MinimumBlockSize : total;
    }

    static void* Payload(Block* block)
    {
        return reinterpret_cast<uint8_t*>(block) + HeaderSize;
    }

    static void CopyBytes(void* destination, const void* source, size_t count)
    {
        auto* to = static_cast<uint8_t*>(destination);
        const auto* from = static_cast<const uint8_t*>(source);
        for (size_t index = 0; index < count; ++index)
        {
            to[index] = from[index];
        }
    }

    static Block* BlockOf(void* pointer)
    {
        if (pointer == nullptr || reinterpret_cast<uintptr_t>(pointer) % Alignment != 0)
        {
            return nullptr;
        }
        auto* block = reinterpret_cast<Block*>(static_cast<uint8_t*>(pointer) - HeaderSize);
        return block->magic == UsedMagic ? block : nullptr;
    }

    // Splits off the tail of block beyond needed as a free block, if the tail is big enough.
    void Split(Block* block, size_t needed)
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

    Block* TakeFreeBlock(size_t needed)
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

    bool Remove(Block* target)
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

    static bool Adjacent(const Block* first, const Block* second)
    {
        return reinterpret_cast<const uint8_t*>(first) + first->size ==
               reinterpret_cast<const uint8_t*>(second);
    }

    void Insert(Block* block)
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

    // The free block that ends where the last region's end marker starts, if there is one.
    Block* TailFreeBlock() const
    {
        Block* last = free_list;
        while (last != nullptr && last->next != nullptr)
        {
            last = last->next;
        }
        return last != nullptr && last_marker != nullptr && Adjacent(last, last_marker) ? last
                                                                                        : nullptr;
    }

    size_t TailFreeBytes() const
    {
        const Block* tail = TailFreeBlock();
        return tail == nullptr ? 0 : tail->size;
    }

    // The end marker moves down to the new end, so the region stays closed off.
    void ReleaseTail()
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

    bool Extend(size_t needed)
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
        if (last_marker != nullptr &&
            reinterpret_cast<uint8_t*>(last_marker) + EndMarkerSize == memory)
        {
            region = last_marker;
            region->size = bytes;
        }
        last_marker = marker;
        Insert(region);
        return true;
    }

    Grow grow;
    Shrink shrink;
    void* context;
    Block* free_list;
    Block* last_marker;
};

} // namespace FeatherOS::LibC

#endif
