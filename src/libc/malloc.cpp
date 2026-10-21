#include "libc/heap_allocator.h"
#include "user/syscall.h"

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

namespace
{

using FeatherOS::LibC::HeapAllocator;

HeapAllocator allocator;
bool initialized = false;

// Grows the program break, first padding it to the allocator's alignment.
void* GrowHeap(size_t bytes, void*)
{
    const uint64_t current = FeatherOS::User::ExtendHeap(0);
    if (FeatherOS::User::IsSyscallError(current))
    {
        return nullptr;
    }
    const uint64_t padding =
        (HeapAllocator::Alignment - current % HeapAllocator::Alignment) % HeapAllocator::Alignment;
    if (bytes > SIZE_MAX - padding)
    {
        return nullptr;
    }
    const uint64_t previous = FeatherOS::User::ExtendHeap(padding + bytes);
    if (FeatherOS::User::IsSyscallError(previous))
    {
        return nullptr;
    }
    return reinterpret_cast<void*>(previous + padding);
}

HeapAllocator& Heap()
{
    if (!initialized)
    {
        allocator.Initialize(GrowHeap, nullptr);
        initialized = true;
    }
    return allocator;
}

// Freeing memory malloc did not hand out means the heap can no longer be trusted.
[[noreturn]] void InvalidPointer(const char* function)
{
    constexpr char Message[] = ": invalid pointer\n";
    write(STDERR_FILENO, function, strlen(function));
    write(STDERR_FILENO, Message, sizeof(Message) - 1);
    abort();
}

} // namespace

void* malloc(size_t size)
{
    void* pointer = Heap().Allocate(size);
    if (pointer == nullptr)
    {
        errno = ENOMEM;
    }
    return pointer;
}

void free(void* pointer)
{
    if (pointer != nullptr && !Heap().Free(pointer))
    {
        InvalidPointer("free");
    }
}

void* calloc(size_t count, size_t size)
{
    if (size != 0 && count > SIZE_MAX / size)
    {
        errno = ENOMEM;
        return nullptr;
    }
    void* pointer = malloc(count * size);
    if (pointer != nullptr)
    {
        memset(pointer, 0, count * size);
    }
    return pointer;
}

// As in glibc, a size of 0 frees the allocation and returns nullptr.
void* realloc(void* pointer, size_t size)
{
    if (pointer == nullptr)
    {
        return malloc(size);
    }
    if (size == 0)
    {
        free(pointer);
        return nullptr;
    }
    bool invalid;
    void* resized = Heap().Resize(pointer, size, &invalid);
    if (invalid)
    {
        InvalidPointer("realloc");
    }
    if (resized == nullptr)
    {
        errno = ENOMEM;
    }
    return resized;
}
