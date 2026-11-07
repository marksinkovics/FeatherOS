import _Concurrency

// The functions the runtime calls to reach executors written in Swift. Upstream writes them as
// generic functions over the executor's type, which Embedded Swift emits only into programs that
// call them; here an executor reference's two words, its object and witness table, become an
// existential. The runtime also passes the executor's type, which an existential does not need.

private func serialExecutor(_ identity: UnsafeRawPointer, _ witnessTable: UnsafeRawPointer)
    -> any SerialExecutor
{
    unsafe unsafeBitCast((identity, witnessTable), to: (any SerialExecutor).self)
}

private func taskExecutor(_ identity: UnsafeRawPointer, _ witnessTable: UnsafeRawPointer)
    -> any TaskExecutor
{
    unsafe unsafeBitCast((identity, witnessTable), to: (any TaskExecutor).self)
}

@_silgen_name("_swift_task_enqueueOnExecutor")
public func enqueueOnExecutor(
    _ job: UnownedJob, _ identity: UnsafeRawPointer, _ witnessTable: UnsafeRawPointer
) {
    serialExecutor(identity, witnessTable).enqueue(ExecutorJob(job))
}

@_silgen_name("_swift_task_enqueueOnTaskExecutor")
public func enqueueOnTaskExecutor(
    _ job: UnownedJob, _ identity: UnsafeRawPointer, _: UnsafeRawPointer,
    _ witnessTable: UnsafeRawPointer
) {
    taskExecutor(identity, witnessTable).enqueue(ExecutorJob(job))
}

@_silgen_name("_task_serialExecutor_checkIsolated")
public func checkIsolated(
    _ identity: UnsafeRawPointer, _: UnsafeRawPointer, _ witnessTable: UnsafeRawPointer
) {
    serialExecutor(identity, witnessTable).checkIsolated()
}

// The runtime's IsIsolatingCurrentContextDecision.
@_silgen_name("_task_serialExecutor_isIsolatingCurrentContext")
public func isIsolatingCurrentContext(
    _ identity: UnsafeRawPointer, _: UnsafeRawPointer, _ witnessTable: UnsafeRawPointer
) -> Int8 {
    switch serialExecutor(identity, witnessTable).isIsolatingCurrentContext() {
    case nil: -1
    case false: 0
    case true: 1
    }
}

// The executor's own reference carries the flags it chose, such as complex equality.
@_silgen_name("_task_serialExecutor_getExecutorRef")
public func serialExecutorReference(
    _ identity: UnsafeRawPointer, _: UnsafeRawPointer, _ witnessTable: UnsafeRawPointer
) -> UnownedSerialExecutor {
    unsafe serialExecutor(identity, witnessTable).asUnownedSerialExecutor()
}

@_silgen_name("_task_taskExecutor_getTaskExecutorRef")
public func taskExecutorReference(
    _ identity: UnsafeRawPointer, _: UnsafeRawPointer, _ witnessTable: UnsafeRawPointer
) -> UnownedTaskExecutor {
    unsafe taskExecutor(identity, witnessTable).asUnownedTaskExecutor()
}
