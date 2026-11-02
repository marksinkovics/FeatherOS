#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

// What the toolchain's x86-64 libswift_Concurrency.a needs from the platform. The archive was
// compiled against Darwin's headers, so it calls some C library functions by their Darwin names.

namespace
{

[[noreturn]] void Unsupported(const char* what)
{
    fprintf(stderr, "Swift concurrency: %s is not supported\n", what);
    abort();
}

// SerialExecutorRef and TaskExecutorRef: an object and a witness table with flags in its low
// bits. Two words come back in RAX and RDX under both the C and the Swift calling conventions,
// and the hooks below take only pointers, so GCC's C functions can stand in for swiftcc ones.
struct ExecutorReference
{
    void* identity;
    uintptr_t implementation;
};

} // namespace

extern "C" int* __error()
{
    return &errno;
}

extern "C" FILE* __stderrp;
FILE* __stderrp = stderr;

extern "C" int _fputs(const char* text, FILE* stream)
{
    return fputs(text, stream);
}

extern "C" int _nanosleep(const struct timespec* duration, struct timespec* remaining)
{
    return nanosleep(duration, remaining);
}

// C11 Annex K: unlike memset, a call the compiler may not remove.
extern "C" int memset_s(void* destination, size_t size, int value, size_t count)
{
    constexpr size_t MaximumSize = SIZE_MAX >> 1;
    if (destination == nullptr || size > MaximumSize)
    {
        return EINVAL;
    }
    memset(destination, value, count < size ? count : size);
    asm volatile("" : : "r"(destination) : "memory");
    return count > size ? EINVAL : 0;
}

// The cooperative executor's deadlines: libc++'s steady_clock::now(), whose time_point holds
// nanoseconds in one 64-bit integer.
extern "C" int64_t SteadyClockNow() __asm__("_ZNSt3__16chrono12steady_clock3nowEv");
extern "C" int64_t SteadyClockNow()
{
    struct timespec now = {0, 0};
    clock_gettime(CLOCK_MONOTONIC, &now);
    constexpr int64_t NanosecondsPerSecond = 1000000000;
    return now.tv_sec * NanosecondsPerSecond + now.tv_nsec;
}

// An Embedded Swift object starts with its class metadata.
extern "C" const void* swift_getObjectType(const void* object)
{
    return *static_cast<const void* const*>(object);
}

extern "C" void swift_reportError(uint32_t, const char* message)
{
    fputs(message, stderr);
}

// The cooperative executor runs the main actor's jobs with the rest, so the main executor is
// the generic one, as its swift_task_getMainExecutorImpl says.
extern "C" ExecutorReference swift_getMainExecutor()
{
    return {nullptr, 0};
}

// Generic Swift functions in the _Concurrency module that call a custom executor. Embedded
// Swift emits generic code only into the programs that use it, so the archive has no copy; they
// are reached only through a custom executor.
extern "C" void _task_serialExecutor_checkIsolated(void*, const void*, const void*)
{
    Unsupported("a custom serial executor");
}

extern "C" int8_t _task_serialExecutor_isIsolatingCurrentContext(void*, const void*, const void*)
{
    Unsupported("a custom serial executor");
}

extern "C" ExecutorReference _task_serialExecutor_getExecutorRef(void*, const void*, const void*)
{
    Unsupported("a custom serial executor");
}

extern "C" ExecutorReference _task_taskExecutor_getTaskExecutorRef(void*, const void*, const void*)
{
    Unsupported("a task executor");
}

// Reached only by distributed actors, which Embedded Swift does not have.
extern "C" [[noreturn]] void GetResilientMetadataBounds(const void*) __asm__(
    "_ZN5swift26getResilientMetadataBoundsEPKNS_21TargetClassDescriptorINS_9InProcessEEE");
extern "C" void GetResilientMetadataBounds(const void*)
{
    Unsupported("a distributed actor");
}
