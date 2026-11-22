#ifndef FEATHEROS_MEMORY_HEAP_H
#define FEATHEROS_MEMORY_HEAP_H

#include <new>
#include <stddef.h>
#include <stdint.h>
#include <type_traits>

namespace FeatherOS::Memory
{

bool InitializeHeap();
void* Allocate(size_t size);
void* AllocateZeroed(size_t count, size_t size);
bool Deallocate(void* pointer);
// The pages the heap has mapped so far; it never unmaps them.
uint64_t HeapMappedBytes();

// A zero-filled T on the heap for one scope, for what would crowd a 16 KiB kernel stack along a
// deep call chain, such as paths and directory entries. Allocation can fail, so check it first.
template <typename T> class HeapObject
{
    static_assert(std::is_trivially_default_constructible_v<T> &&
                  std::is_trivially_destructible_v<T>);

public:
    HeapObject() : object(static_cast<T*>(AllocateZeroed(1, sizeof(T)))) {}
    ~HeapObject()
    {
        if (object != nullptr)
        {
            Deallocate(object);
        }
    }
    HeapObject(const HeapObject&) = delete;
    HeapObject& operator=(const HeapObject&) = delete;
    HeapObject(HeapObject&&) = delete;
    HeapObject& operator=(HeapObject&&) = delete;

    explicit operator bool() const
    {
        return object != nullptr;
    }
    T& operator*() const
    {
        return *object;
    }
    T* operator->() const
    {
        return object;
    }

private:
    T* object;
};

} // namespace FeatherOS::Memory

extern "C"
{

    void* malloc(size_t size);
    void* calloc(size_t count, size_t size);
    void free(void* pointer);
}

#endif