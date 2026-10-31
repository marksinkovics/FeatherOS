#include "kernel/string/memory_functions.h"

#include <stdint.h>

// Built at -O0; with optimization, GCC could turn these loops back into calls to themselves
// unless the file is built with -fno-tree-loop-distribute-patterns.
extern "C"
{

    void* memcpy(void* destination, const void* source, size_t size)
    {
        auto* to = static_cast<uint8_t*>(destination);
        const auto* from = static_cast<const uint8_t*>(source);
        for (size_t index = 0; index < size; ++index)
        {
            to[index] = from[index];
        }
        return destination;
    }

    void* memmove(void* destination, const void* source, size_t size)
    {
        auto* to = static_cast<uint8_t*>(destination);
        const auto* from = static_cast<const uint8_t*>(source);
        if (to < from)
        {
            return memcpy(destination, source, size);
        }
        for (size_t index = size; index > 0; --index)
        {
            to[index - 1] = from[index - 1];
        }
        return destination;
    }

    void* memset(void* destination, int value, size_t size)
    {
        auto* to = static_cast<uint8_t*>(destination);
        for (size_t index = 0; index < size; ++index)
        {
            to[index] = static_cast<uint8_t>(value);
        }
        return destination;
    }

    int memcmp(const void* first, const void* second, size_t size)
    {
        const auto* left = static_cast<const uint8_t*>(first);
        const auto* right = static_cast<const uint8_t*>(second);
        for (size_t index = 0; index < size; ++index)
        {
            if (left[index] != right[index])
            {
                return left[index] < right[index] ? -1 : 1;
            }
        }
        return 0;
    }
}
