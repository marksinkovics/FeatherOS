#include "libc/exit_handlers.h"
#include "libc/lock.h"
#include "user/syscall.h"

#include <stddef.h>
#include <stdint.h>
#include <signal.h>
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
// What a shell shows for SIGABRT, 128 + 6, if the signal somehow did not end the program.
constexpr int AbortStatus = 134;

ExitHandler handlers[MaximumExitHandlers];
size_t handler_count = 0;
FeatherOS::LibC::Lock handlers_lock;
void (*exit_flush)() = nullptr;

// thread_local objects with destructors, newest first, for the thread that built them.
struct ThreadExitHandler
{
    void (*function)(void*);
    void* argument;
    ThreadExitHandler* next;
};

__thread ThreadExitHandler* thread_exit_handlers = nullptr;

bool AddHandler(const ExitHandler& handler)
{
    const FeatherOS::LibC::Guard<FeatherOS::LibC::Lock> guard(handlers_lock);
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

void SetExitFlush(void (*flush)())
{
    exit_flush = flush;
}

bool AddThreadExitHandler(void (*function)(void*), void* argument)
{
    auto* handler = static_cast<ThreadExitHandler*>(malloc(sizeof(ThreadExitHandler)));
    if (handler == nullptr)
    {
        return false;
    }
    *handler = {function, argument, thread_exit_handlers};
    thread_exit_handlers = handler;
    return true;
}

// A destructor may build another thread_local object, so the list is read again each time.
void RunThreadExitHandlers()
{
    while (thread_exit_handlers != nullptr)
    {
        ThreadExitHandler* handler = thread_exit_handlers;
        thread_exit_handlers = handler->next;
        handler->function(handler->argument);
        free(handler);
    }
}

} // namespace FeatherOS::LibC

extern "C" int atexit(void (*function)())
{
    return AddHandler({nullptr, function, nullptr}) ? 0 : -1;
}

extern "C" void exit(int status)
{
    // The calling thread's thread_local objects go first, as C++ requires.
    FeatherOS::LibC::RunThreadExitHandlers();
    // Handlers run newest first; one may register another, which then runs next.
    for (;;)
    {
        ExitHandler handler;
        {
            const FeatherOS::LibC::Guard<FeatherOS::LibC::Lock> guard(handlers_lock);
            if (handler_count == 0)
            {
                break;
            }
            handler = handlers[--handler_count];
        }
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
    // Streams are flushed last, so output from handlers and destructors is not lost.
    if (exit_flush != nullptr)
    {
        exit_flush();
    }
    _Exit(status);
}

// Only the low 8 bits reach the parent, as in POSIX. This also keeps a program's status apart
// from the kernel's fault statuses near UINT64_MAX.
extern "C" void _Exit(int status)
{
    FeatherOS::User::Exit(static_cast<uint8_t>(status));
}

// As POSIX asks: a SIGABRT handler runs first, but the program ends even if it returns.
extern "C" void abort()
{
    sigset_t abort_signal = 0;
    sigaddset(&abort_signal, SIGABRT);
    sigprocmask(SIG_UNBLOCK, &abort_signal, nullptr);
    raise(SIGABRT);
    signal(SIGABRT, SIG_DFL);
    raise(SIGABRT);
    _Exit(AbortStatus);
}
