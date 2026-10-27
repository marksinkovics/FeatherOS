#ifndef FEATHEROS_MEMORY_HEAP_H
#define FEATHEROS_MEMORY_HEAP_H

#include <stddef.h>
#include <stdint.h>

namespace FeatherOS::Memory
{

bool InitializeHeap();
void* Allocate(size_t size);
void* AllocateZeroed(size_t count, size_t size);
bool Deallocate(void* pointer);
// The pages the heap has mapped so far; it never unmaps them.
uint64_t HeapMappedBytes();

} // namespace FeatherOS::Memory

extern "C"
{

    void* malloc(size_t size);
    void* calloc(size_t count, size_t size);
    void free(void* pointer);
}

void* operator new(size_t size);
void* operator new[](size_t size);
void operator delete(void* pointer) noexcept;
void operator delete[](void* pointer) noexcept;
void operator delete(void* pointer, size_t size) noexcept;
void operator delete[](void* pointer, size_t size) noexcept;

#endif