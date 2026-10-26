#include "syscall_result.h"
#include "user/syscall.h"

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>

namespace
{

// The main thread counts too, so the last thread to call pthread_exit knows to end the process.
int live_threads = 1;

[[noreturn]] void EndThread(void* value)
{
    if (__atomic_sub_fetch(&live_threads, 1, __ATOMIC_ACQ_REL) == 0)
    {
        exit(0);
    }
    FeatherOS::User::Syscall(FeatherOS::User::SyscallNumber::ThreadExit,
                             reinterpret_cast<uint64_t>(value));
    __builtin_unreachable();
}

// Every new thread starts here, with block pointing to {start, argument} at its stack's top.
extern "C" [[noreturn]] void FeatherOSThreadStart(const uint64_t* block)
{
    auto* start = reinterpret_cast<void* (*)(void*)>(block[0]);
    EndThread(start(reinterpret_cast<void*>(block[1])));
}

} // namespace

int pthread_create(pthread_t* thread, const pthread_attr_t*, void* (*start)(void* argument),
                   void* argument)
{
    if (thread == nullptr || start == nullptr)
    {
        return EINVAL;
    }
    __atomic_add_fetch(&live_threads, 1, __ATOMIC_ACQ_REL);
    const uint64_t result = FeatherOS::User::Syscall(
        FeatherOS::User::SyscallNumber::ThreadCreate,
        reinterpret_cast<uint64_t>(&FeatherOSThreadStart), reinterpret_cast<uint64_t>(start),
        reinterpret_cast<uint64_t>(argument));
    if (FeatherOS::User::IsSyscallError(result))
    {
        __atomic_sub_fetch(&live_threads, 1, __ATOMIC_ACQ_REL);
        return result == FeatherOS::User::SyscallErrorWouldBlock
                   ? EAGAIN
                   : FeatherOS::LibC::ErrorNumber(result);
    }
    *thread = result;
    return 0;
}

int pthread_join(pthread_t thread, void** value)
{
    if (thread == pthread_self())
    {
        return EDEADLK;
    }
    uint64_t result = 0;
    const uint64_t status = FeatherOS::User::Syscall(FeatherOS::User::SyscallNumber::ThreadJoin,
                                                     thread, reinterpret_cast<uint64_t>(&result));
    if (status == FeatherOS::User::SyscallErrorNotFound)
    {
        return ESRCH;
    }
    if (FeatherOS::User::IsSyscallError(status))
    {
        return FeatherOS::LibC::ErrorNumber(status);
    }
    if (value != nullptr)
    {
        *value = reinterpret_cast<void*>(result);
    }
    return 0;
}

void pthread_exit(void* value)
{
    EndThread(value);
}

pthread_t pthread_self(void)
{
    return FeatherOS::User::Syscall(FeatherOS::User::SyscallNumber::ThreadId);
}

int pthread_equal(pthread_t first, pthread_t second)
{
    return first == second ? 1 : 0;
}

int pthread_attr_init(pthread_attr_t* attributes)
{
    attributes->unused = 0;
    return 0;
}

int pthread_attr_destroy(pthread_attr_t*)
{
    return 0;
}
