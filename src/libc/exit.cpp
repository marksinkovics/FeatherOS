#include "libc/exit_handlers.h"
#include "user/syscall.h"

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

namespace
{

using FiniFunction = void (*)();

struct ExitHandler
{
    void (*with_argument)(void*);
    void (*plain)();
    void* argument;
};

// The C standard requires room for at least 32 handlers.
constexpr size_t MaximumExitHandlers = 32;
// No signals yet, so abort reports what a shell shows for SIGABRT: 128 + 6.
constexpr int AbortStatus = 134;

ExitHandler handlers[MaximumExitHandlers];
size_t handler_count = 0;

bool AddHandler(const ExitHandler& handler)
{
    if (handler_count == MaximumExitHandlers)
    {
        return false;
    }
    handlers[handler_count++] = handler;
    return true;
}

} // namespace

extern "C" const FiniFunction __fini_array_start[] __attribute__((visibility("hidden")));
extern "C" const FiniFunction __fini_array_end[] __attribute__((visibility("hidden")));

namespace FeatherOS::LibC
{

bool AddExitHandler(void (*function)(void*), void* argument)
{
    return AddHandler({function, nullptr, argument});
}

} // namespace FeatherOS::LibC

extern "C" int atexit(void (*function)())
{
    return AddHandler({nullptr, function, nullptr}) ? 0 : -1;
}

extern "C" void exit(int status)
{
    // Handlers run newest first; one may register another, which then runs next.
    while (handler_count > 0)
    {
        const ExitHandler handler = handlers[--handler_count];
        if (handler.plain != nullptr)
        {
            handler.plain();
        }
        else
        {
            handler.with_argument(handler.argument);
        }
    }
    for (const FiniFunction* function = __fini_array_end; function != __fini_array_start;)
    {
        (*--function)();
    }
    _Exit(status);
}

// Only the low 8 bits reach the parent, as in POSIX. This also keeps a program's status apart
// from the kernel's fault statuses near UINT64_MAX.
extern "C" void _Exit(int status)
{
    FeatherOS::User::Exit(static_cast<uint8_t>(status));
}

extern "C" void abort()
{
    _Exit(AbortStatus);
}
