#include "thread_local_storage.h"

namespace FeatherOS::ThreadLocalStorage
{
namespace
{

uint64_t AlignDown(uint64_t value, uint64_t alignment)
{
    return value & ~(alignment - 1);
}

uint64_t AlignUp(uint64_t value, uint64_t alignment)
{
    return (value + alignment - 1) & ~(alignment - 1);
}

} // namespace

bool Place(Variant variant, uint64_t top, uint64_t memory_size, uint64_t alignment, Layout* layout)
{
    const uint64_t pointer_alignment = alignment > ControlBlockSize ? alignment : ControlBlockSize;
    const uint64_t block_size = AlignUp(memory_size, alignment);
    const uint64_t control_offset = AlignUp(ControlBlockSize, alignment);
    const uint64_t needed = ControlBlockSize + block_size + control_offset + pointer_alignment;
    if (alignment == 0 || (alignment & (alignment - 1)) != 0 || top < needed)
    {
        return false;
    }
    if (variant == Variant::BlockBelowPointer)
    {
        layout->thread_pointer = AlignDown(top - ControlBlockSize, pointer_alignment);
        layout->block = layout->thread_pointer - block_size;
        layout->bottom = AlignDown(layout->block, ControlBlockSize);
        return true;
    }
    layout->thread_pointer = AlignDown(top - control_offset - memory_size, pointer_alignment);
    layout->block = layout->thread_pointer + control_offset;
    layout->bottom = layout->thread_pointer;
    return true;
}

} // namespace FeatherOS::ThreadLocalStorage
