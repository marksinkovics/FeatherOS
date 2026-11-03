#include "swift/ABI/Executor.h"
#include "swift/Runtime/Casting.h"
#include "swift/Runtime/Debug.h"
#include "swift/Runtime/HeapObject.h"
#include "swift/Runtime/Metadata.h"

#include <chrono>
#include <thread>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

// What the concurrency runtime needs that neither the C library nor the Embedded Swift standard
// library provides: two functions from libc++'s compiled library, which FeatherOS does not
// have, and runtime entry points that Embedded Swift leaves out.

using namespace swift;

namespace
{

[[noreturn]] void Unsupported(const char* what)
{
    fprintf(stderr, "Swift concurrency: %s is not supported\n", what);
    abort();
}

} // namespace

// The cooperative executor's deadlines and its wait for the next delayed job.
std::chrono::steady_clock::time_point std::chrono::steady_clock::now() noexcept
{
    struct timespec now = {};
    clock_gettime(CLOCK_MONOTONIC, &now);
    return time_point(seconds(now.tv_sec) + nanoseconds(now.tv_nsec));
}

void std::this_thread::sleep_for(const std::chrono::nanoseconds& duration)
{
    if (duration <= std::chrono::nanoseconds::zero())
    {
        return;
    }
    const auto whole = std::chrono::duration_cast<std::chrono::seconds>(duration);
    struct timespec remaining = {static_cast<time_t>(whole.count()),
                                 static_cast<long>((duration - whole).count())};
    while (nanosleep(&remaining, &remaining) == -1 && errno == EINTR)
    {
    }
}

// An Embedded Swift object starts with its class metadata.
const Metadata* swift::swift_getObjectType(HeapObject* object)
{
    return object->metadata;
}

void swift::swift_reportError(uint32_t, const char* message)
{
    fputs(message, stderr);
}

// Executor references are two words, returned in registers as the Swift side expects.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wreturn-type-c-linkage"

// The cooperative executor runs the main actor's jobs with the rest, so the main executor is
// the generic one, as its swift_task_getMainExecutorImpl says. The Swift version of this
// function is built only for WASI or without Embedded Swift.
extern "C" SWIFT_CC(swift) SerialExecutorRef swift_getMainExecutor()
{
    return SerialExecutorRef::generic();
}

// Generic Swift functions in the _Concurrency module that call a custom executor. Embedded
// Swift emits generic code only into the programs that use it, so the runtime has no copy; they
// are reached only through a custom executor.

extern "C" SWIFT_CC(swift) void _task_serialExecutor_checkIsolated(
    HeapObject*, const Metadata*, const SerialExecutorWitnessTable*)
{
    Unsupported("a custom serial executor");
}

extern "C" SWIFT_CC(swift) int8_t
    _task_serialExecutor_isIsolatingCurrentContext(HeapObject*, const Metadata*,
                                                   const SerialExecutorWitnessTable*)
{
    Unsupported("a custom serial executor");
}

extern "C" SWIFT_CC(swift) SerialExecutorRef
    _task_serialExecutor_getExecutorRef(HeapObject*, const Metadata*,
                                        const SerialExecutorWitnessTable*)
{
    Unsupported("a custom serial executor");
}

extern "C" SWIFT_CC(swift) TaskExecutorRef
    _task_taskExecutor_getTaskExecutorRef(HeapObject*, const Metadata*,
                                          const TaskExecutorWitnessTable*)
{
    Unsupported("a task executor");
}

#pragma clang diagnostic pop

// Reached only by distributed actors, which Embedded Swift does not have.
ClassMetadataBounds swift::getResilientMetadataBounds(const ClassDescriptor*)
{
    Unsupported("a distributed actor");
}
