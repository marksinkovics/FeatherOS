#ifndef FEATHEROS_LIBC_HEAP_SORT_H
#define FEATHEROS_LIBC_HEAP_SORT_H

#include <stddef.h>

namespace FeatherOS::LibC
{

using Comparison = int (*)(const void*, const void*);

inline void SwapElements(unsigned char* left, unsigned char* right, size_t size)
{
    for (size_t index = 0; index < size; ++index)
    {
        const unsigned char byte = left[index];
        left[index] = right[index];
        right[index] = byte;
    }
}

inline void SiftDown(unsigned char* base, size_t root, size_t count, size_t size,
                     Comparison compare)
{
    for (;;)
    {
        size_t largest = root;
        const size_t left = 2 * root + 1;
        const size_t right = left + 1;
        if (left < count && compare(base + left * size, base + largest * size) > 0)
        {
            largest = left;
        }
        if (right < count && compare(base + right * size, base + largest * size) > 0)
        {
            largest = right;
        }
        if (largest == root)
        {
            return;
        }
        SwapElements(base + root * size, base + largest * size, size);
        root = largest;
    }
}

// Heapsort needs no recursion or extra memory and is O(n log n) even on adversarial input, which
// matters more here than the speed of quicksort's average case.
inline void HeapSort(void* elements, size_t count, size_t size, Comparison compare)
{
    auto* base = static_cast<unsigned char*>(elements);
    if (count < 2 || size == 0)
    {
        return;
    }
    for (size_t root = count / 2; root > 0; --root)
    {
        SiftDown(base, root - 1, count, size, compare);
    }
    for (size_t end = count - 1; end > 0; --end)
    {
        SwapElements(base, base + end * size, size);
        SiftDown(base, 0, end, size, compare);
    }
}

} // namespace FeatherOS::LibC

#endif
