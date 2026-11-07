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

#pragma clang diagnostic pop

// Reached only by distributed actors, which Embedded Swift does not have.
ClassMetadataBounds swift::getResilientMetadataBounds(const ClassDescriptor*)
{
    Unsupported("a distributed actor");
}
