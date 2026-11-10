import FeatherLibC
import _Concurrency

// Custom executors: actors whose jobs run on a thread of their own, and a pool of threads that
// tasks can prefer over the global executor. Each check records the threads its code ran on.

// The same source builds as fullexecutors, in full Swift.
#if $Embedded
let program = "executors"
#else
let program = "fullexecutors"
#endif
let mainThread = pthread_self()

func onMainThread() -> Bool {
    pthread_equal(pthread_self(), mainThread) != 0
}

// Work for an executor's threads: a job, or a call that runs outside any job.
enum Work {
    case job(UnownedJob)
    case call(() -> Void)
}

class WorkThreads: @unchecked Sendable {
    private let lock = UnsafeMutablePointer<pthread_mutex_t>.allocate(capacity: 1)
    private let queued = UnsafeMutablePointer<pthread_cond_t>.allocate(capacity: 1)
    private var pending: [Work] = []
    private(set) var threads: [pthread_t] = []

    init(count: Int) {
        pthread_mutex_init(lock, nil)
        pthread_cond_init(queued, nil)
        for _ in 0..<count {
            var thread: pthread_t = 0
            let context = Unmanaged.passUnretained(self).toOpaque()
            guard
                pthread_create(
                    &thread, nil,
                    { context in
                        Unmanaged<WorkThreads>.fromOpaque(context!).takeUnretainedValue().runWork()
                    }, context) == 0
            else {
                fatalError("\(program): could not start a thread")
            }
            threads.append(thread)
        }
    }

    var onThread: Bool {
        threads.contains { pthread_equal($0, pthread_self()) != 0 }
    }

    func enqueue(_ job: consuming ExecutorJob) {
        add(.job(UnownedJob(job)))
    }

    func call(_ body: @escaping () -> Void) {
        add(.call(body))
    }

    func run(_ job: UnownedJob) {
        fatalError("\(program): no way to run a job")
    }

    private func add(_ item: Work) {
        pthread_mutex_lock(lock)
        pending.append(item)
        pthread_cond_signal(queued)
        pthread_mutex_unlock(lock)
    }

    private func runWork() -> UnsafeMutableRawPointer? {
        while true {
            pthread_mutex_lock(lock)
            while pending.isEmpty {
                pthread_cond_wait(queued, lock)
            }
            let item = pending.removeFirst()
            pthread_mutex_unlock(lock)
            switch item {
            case .job(let job): run(job)
            case .call(let body): body()
            }
        }
    }
}

// One thread runs the jobs in order, so the executor is serial. Outside a job, the runtime asks
// it whether the current thread is isolated to it; the one that cannot tell gets checkIsolated.
final class ActorThread: WorkThreads, SerialExecutor, @unchecked Sendable {
    private let answersIsolation: Bool
    private(set) var isolationAnswers = 0
    private(set) var isolationChecks = 0

    init(answersIsolation: Bool) {
        self.answersIsolation = answersIsolation
        super.init(count: 1)
    }

    override func run(_ job: UnownedJob) {
        job.runSynchronously(on: asUnownedSerialExecutor())
    }

    func isIsolatingCurrentContext() -> Bool? {
        guard answersIsolation else {
            return nil
        }
        isolationAnswers += 1
        return onThread
    }

    func checkIsolated() {
        isolationChecks += 1
        precondition(onThread, "\(program): not on the actor's thread")
    }
}

final class Pool: WorkThreads, TaskExecutor, @unchecked Sendable {
    override func run(_ job: UnownedJob) {
        job.runSynchronously(on: asUnownedTaskExecutor())
    }
}

actor Pinned {
    let thread: ActorThread

    init(thread: ActorThread) {
        self.thread = thread
    }

    nonisolated var unownedExecutor: UnownedSerialExecutor {
        thread.asUnownedSerialExecutor()
    }

    func onOwnThread() -> Bool {
        thread.onThread
    }

    func onOwnThreadAfterSleeping() async -> Bool {
        let before = thread.onThread
        try? await Task.sleep(for: .milliseconds(5))
        return before && thread.onThread
    }
}

actor Tally {
    func currentThread() -> pthread_t {
        pthread_self()
    }
}

func currentThread() async -> pthread_t {
    pthread_self()
}

func callsOnOwnThread(_ pinned: Pinned, count: Int) async -> Bool {
    await withTaskGroup(of: Bool.self) { group in
        for _ in 0..<count {
            group.addTask { await pinned.onOwnThread() }
        }
        var all = true
        for await onThread in group {
            all = all && onThread
        }
        return all
    }
}

func assumedOutsideJob(_ pinned: Pinned) async -> Bool {
    await withCheckedContinuation { continuation in
        pinned.thread.call {
            pinned.preconditionIsolated()
            pinned.assertIsolated()
            continuation.resume(returning: pinned.assumeIsolated { $0.onOwnThread() })
        }
    }
}

func groupOnPool(_ pool: Pool, tasks: Int) async -> Bool {
    await withTaskGroup(of: pthread_t.self) { group in
        for _ in 0..<tasks {
            group.addTask(executorPreference: pool) {
                await Task.yield()
                return pthread_self()
            }
        }
        var all = true
        for await thread in group {
            all = all && onPool(pool, thread)
        }
        return all
    }
}

func onPool(_ pool: Pool, _ thread: pthread_t) -> Bool {
    pool.threads.contains { pthread_equal($0, thread) != 0 }
}

func preferringPool(_ pool: Pool, _ pinned: Pinned) async -> String {
    await withTaskExecutorPreference(pool) {
        let function = onPool(pool, await currentThread())
        async let child = currentThread()
        let childOnPool = onPool(pool, await child)
        let defaultActor = onPool(pool, await Tally().currentThread())
        let ownThread = await pinned.onOwnThread()
        let unstructured = onPool(pool, await Task { await currentThread() }.value)
        // No preference: a child task goes to the global executor.
        let global = await withTaskExecutorPreference(globalConcurrentExecutor) {
            async let child = currentThread()
            return onPool(pool, await child)
        }
        return "a function \(function), async let \(childOnPool), a default actor "
            + "\(defaultActor), the pinned actor on its thread \(ownThread), Task {} "
            + "\(unstructured), globalConcurrentExecutor \(global)"
    }
}

let answering = ActorThread(answersIsolation: true)
let checking = ActorThread(answersIsolation: false)
let pinned = Pinned(thread: answering)
let checked = Pinned(thread: checking)
let pool = Pool(count: 2)

let calls = await callsOnOwnThread(pinned, count: 100)
let slept = await pinned.onOwnThreadAfterSleeping()
print("\(program): an actor ran 100 calls on its own thread \(calls),", terminator: " ")
print("resumed there after sleeping \(slept), top-level code on the main thread \(onMainThread())")
let answered = await assumedOutsideJob(pinned) && answering.isolationAnswers > 0
let checkedOutside = await assumedOutsideJob(checked) && checking.isolationChecks > 0
print("\(program): assumeIsolated outside a job:", terminator: " ")
print("isIsolatingCurrentContext \(answered), checkIsolated \(checkedOutside)")
print("\(program): 16 tasks preferring a pool ran there \(await groupOnPool(pool, tasks: 16))")
print("\(program): preferring the pool: \(await preferringPool(pool, pinned))")
let preferred = await Task(executorPreference: pool) { await currentThread() }.value
print("\(program): Task(executorPreference:) on the pool \(onPool(pool, preferred))")
