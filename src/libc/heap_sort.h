#ifndef FEATHEROS_LIBC_HEAP_SORT_H
#define FEATHEROS_LIBC_HEAP_SORT_H

#include <stddef.h>

namespace FeatherOS::LibC
{

using Comparison = int (*)(const void*, const void*);
void SwapElements(unsigned char* left, unsigned char* right, size_t size);
void SiftDown(unsigned char* base, size_t root, size_t count, size_t size, Comparison compare);

// Heapsort needs no recursion or extra memory and is O(n log n) even on adversarial input, which
// matters more here than the speed of quicksort's average case.
void HeapSort(void* elements, size_t count, size_t size, Comparison compare);

} // namespace FeatherOS::LibC

#endif
