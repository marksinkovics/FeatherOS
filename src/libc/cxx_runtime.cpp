#include "libc/exit_handlers.h"
#include "libc/lock.h"

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

extern "C" int __cxa_thread_atexit(void (*destructor)(void*), void* object, void*)
{
    return FeatherOS::LibC::AddThreadExitHandler(destructor, object) ? 0 : -1;
}

// Guards a function-local static's first initialization. The compiler only checks the first
// byte, which says it is done; the word at offset 4 says whether a thread is running the
// initializer (1), with others asleep waiting for it (2).
namespace
{

uint8_t* GuardDone(int64_t* guard)
{
    return reinterpret_cast<uint8_t*>(guard);
}

int* GuardState(int64_t* guard)
{
    return reinterpret_cast<int*>(guard) + 1;
}

} // namespace

extern "C" int __cxa_guard_acquire(int64_t* guard)
{
    int* state = GuardState(guard);
    for (;;)
    {
        if (__atomic_load_n(GuardDone(guard), __ATOMIC_ACQUIRE) != 0)
        {
            return 0;
        }
        int expected = 0;
        if (__atomic_compare_exchange_n(state, &expected, 1, false, __ATOMIC_ACQUIRE,
                                        __ATOMIC_ACQUIRE))
        {
            // Another thread may have finished just before.
            if (__atomic_load_n(GuardDone(guard), __ATOMIC_ACQUIRE) != 0)
            {
                __atomic_store_n(state, 0, __ATOMIC_RELEASE);
                return 0;
            }
            return 1;
        }
        if (expected == 1)
        {
            __atomic_compare_exchange_n(state, &expected, 2, false, __ATOMIC_ACQUIRE,
                                        __ATOMIC_ACQUIRE);
        }
        if (expected != 0)
        {
            FeatherOS::LibC::FutexWait(state, 2);
        }
    }
}

extern "C" void __cxa_guard_release(int64_t* guard)
{
    __atomic_store_n(GuardDone(guard), 1, __ATOMIC_RELEASE);
    if (__atomic_exchange_n(GuardState(guard), 0, __ATOMIC_RELEASE) == 2)
    {
        FeatherOS::LibC::FutexWake(GuardState(guard), INT32_MAX);
    }
}

// The initializer failed, so a waiting thread tries it instead.
extern "C" void __cxa_guard_abort(int64_t* guard)
{
    if (__atomic_exchange_n(GuardState(guard), 0, __ATOMIC_RELEASE) == 2)
    {
        FeatherOS::LibC::FutexWake(GuardState(guard), INT32_MAX);
    }
}

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
