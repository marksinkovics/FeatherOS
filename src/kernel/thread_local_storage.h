#ifndef FEATHEROS_THREAD_LOCAL_STORAGE_H
#define FEATHEROS_THREAD_LOCAL_STORAGE_H

#include <stdint.h>

// Where a thread's TLS block and thread pointer go, at the top of its stack slot. The two ELF
// TLS variants differ: on x86-64 the block ends at the thread pointer, whose first word points
// to itself; on AArch64 the pointer is followed by a 16-byte control block and then the block.
// The linker computes every variable's offset from the pointer this way.
namespace FeatherOS::ThreadLocalStorage
{

enum class Variant : uint8_t
{
    BlockBelowPointer,
    BlockAbovePointer,
};

constexpr uint64_t ControlBlockSize = 16;

struct Layout
{
    uint64_t thread_pointer;
    uint64_t block;
    // The lowest byte the area uses, 16-byte aligned; the thread's stack starts below it.
    uint64_t bottom;
};

// alignment is a power of two; false when the area would not fit below top.
bool Place(Variant variant, uint64_t top, uint64_t memory_size, uint64_t alignment, Layout* layout);

} // namespace FeatherOS::ThreadLocalStorage

#endif
