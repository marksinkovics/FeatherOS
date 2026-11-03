#include "swift/ABI/Executor.h"
#include "swift/Runtime/Casting.h"
#include "swift/Runtime/Concurrency.h"
#include "swift/Runtime/Debug.h"
#include "swift/Runtime/HeapObject.h"
#include "swift/Runtime/Metadata.h"

#include <stdio.h>
#include <stdlib.h>

// Runtime entry points that the concurrency runtime needs and Embedded Swift leaves out.

using namespace swift;

namespace
{

[[noreturn]] void Unsupported(const char* what)
{
    fprintf(stderr, "Swift concurrency: %s is not supported\n", what);
    abort();
}

} // namespace

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

// The Swift version of this function is built only for WASI or without Embedded Swift.
extern "C" SWIFT_CC(swift) SerialExecutorRef swift_getMainExecutor()
{
    return swift_task_getMainExecutor();
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
