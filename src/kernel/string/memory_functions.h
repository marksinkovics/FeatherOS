#ifndef FEATHEROS_STRING_MEMORY_FUNCTIONS_H
#define FEATHEROS_STRING_MEMORY_FUNCTIONS_H

#include <stddef.h>

// GCC may emit calls to these four even in a freestanding build, for struct copies and
// initializers, so the kernel has to provide them.
extern "C"
{
    void* memcpy(void* destination, const void* source, size_t size);
    void* memmove(void* destination, const void* source, size_t size);
    void* memset(void* destination, int value, size_t size);
    int memcmp(const void* first, const void* second, size_t size);
}

#endif
