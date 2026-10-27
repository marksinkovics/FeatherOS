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
    void Initialize(Grow grow_function, void* grow_context, Shrink shrink_function = nullptr);
    // nullptr if size does not fit, or if Grow could not provide the memory.
    void* Allocate(size_t size);
    // False, and nothing changes, for a pointer this allocator did not hand out or that was
    // already freed.
    bool Free(void* pointer);
    // Grows or shrinks an allocation in place when it can, and otherwise moves it. Returns the
    // new pointer, or nullptr (leaving the old allocation intact) if there is no memory.
    void* Resize(void* pointer, size_t size, bool* invalid);
    // The bytes the caller may use, at least what it asked for; 0 for an invalid pointer.
    size_t UsableSize(void* pointer);

    size_t FreeBytes() const;
    size_t FreeBlockCount() const;

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
    static size_t BlockSize(size_t size);
    static void* Payload(Block* block);
    static void CopyBytes(void* destination, const void* source, size_t count);
    static Block* BlockOf(void* pointer);
    // Splits off the tail of block beyond needed as a free block, if the tail is big enough.
    void Split(Block* block, size_t needed);

    Block* TakeFreeBlock(size_t needed);
    bool Remove(Block* target);
    static bool Adjacent(const Block* first, const Block* second);
    void Insert(Block* block);
    // The free block that ends where the last region's end marker starts, if there is one.
    Block* TailFreeBlock() const;

    size_t TailFreeBytes() const;
    // The end marker moves down to the new end, so the region stays closed off.
    void ReleaseTail();

    bool Extend(size_t needed);
    Grow grow;
    Shrink shrink;
    void* context;
    Block* free_list;
    Block* last_marker;
};

} // namespace FeatherOS::LibC

#endif
