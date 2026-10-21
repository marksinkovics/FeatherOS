#include "libc/exit_handlers.h"

#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>

// The hooks the C++ compiler expects from its runtime (the Itanium C++ ABI, which both
// architectures use).

// Identifies this program to __cxa_atexit; shared libraries would each have their own. Global
// variables are not name-mangled, so this needs no extern "C".
void* __dso_handle __attribute__((visibility("hidden"))) = nullptr;

extern "C" int __cxa_atexit(void (*destructor)(void*), void* object, void*)
{
    return FeatherOS::LibC::AddExitHandler(destructor, object) ? 0 : -1;
}

// Guards a function-local static's first initialization. Programs are single-threaded, so no
// lock is needed yet; only the guard's first byte is used.
extern "C" int __cxa_guard_acquire(int64_t* guard)
{
    return *reinterpret_cast<uint8_t*>(guard) == 0 ? 1 : 0;
}

extern "C" void __cxa_guard_release(int64_t* guard)
{
    *reinterpret_cast<uint8_t*>(guard) = 1;
}

extern "C" void __cxa_guard_abort(int64_t*) {}

extern "C" void __cxa_pure_virtual()
{
    abort();
}

// With exceptions disabled, a failed allocation cannot throw std::bad_alloc, so it ends the
// program instead of returning nullptr to code that does not check.
void* operator new(size_t size)
{
    void* pointer = malloc(size == 0 ? 1 : size);
    if (pointer == nullptr)
    {
        constexpr char Message[] = "operator new: out of memory\n";
        write(STDERR_FILENO, Message, sizeof(Message) - 1);
        abort();
    }
    return pointer;
}

void* operator new[](size_t size)
{
    return operator new(size);
}

void operator delete(void* pointer) noexcept
{
    free(pointer);
}

void operator delete[](void* pointer) noexcept
{
    free(pointer);
}

// The sized forms C++14 compilers call when they know the object's size.
void operator delete(void* pointer, size_t) noexcept
{
    free(pointer);
}

void operator delete[](void* pointer, size_t) noexcept
{
    free(pointer);
}
