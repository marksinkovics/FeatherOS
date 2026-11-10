#include "Error.h"
#include "ExecutorImpl.h"

// PriorityQueue.h uses numeric_limits without including <limits>.
#include <limits>

#include "swift/Basic/PriorityQueue.h"
#include "swift/Threading/Thread.h"

#include <pthread.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>

// The global executor: one queue of jobs, run by a worker thread for every CPU, which start with
// the first job. MainActor's jobs wait in a queue of their own, which only the main thread runs.
// Embedded Swift reaches it through the runtime's *Impl hooks, in place of upstream's
// single-threaded CooperativeGlobalExecutor.cpp. Full Swift reaches it through the swift_dispatch*
// functions behind upstream's DispatchExecutor.swift, which is otherwise built over Dispatch.

namespace
{

struct JobQueueTraits
{
    static SwiftJob* getNext(SwiftJob* job)
    {
        return static_cast<SwiftJob*>(job->schedulerPrivate[0]);
    }
    static void setNext(SwiftJob* job, SwiftJob* next)
    {
        job->schedulerPrivate[0] = next;
    }
    enum
    {
        prioritiesCount = SwiftJobPriorityBucketCount
    };
    static int getPriorityIndex(SwiftJob* job)
    {
        return swift_priority_getBucketIndex(swift_job_getPriority(job));
    }
};

constexpr uint64_t NanosecondsPerSecond = 1000000000;

struct JobQueue
{
    swift::PriorityQueue<SwiftJob*, JobQueueTraits> ready;
    // Sorted by deadline, then in the order they came.
    SwiftJob* delayed = nullptr;
    // On CLOCK_MONOTONIC, set up by Start.
    pthread_cond_t queued;
};

pthread_once_t started = PTHREAD_ONCE_INIT;
pthread_mutex_t queue_lock = PTHREAD_MUTEX_INITIALIZER;
JobQueue global_queue;
JobQueue main_queue;

uint64_t Now()
{
    struct timespec now = {};
    clock_gettime(CLOCK_MONOTONIC, &now);
    return static_cast<uint64_t>(now.tv_sec) * NanosecondsPerSecond +
           static_cast<uint64_t>(now.tv_nsec);
}

uint64_t DeadlineOf(SwiftJob* job)
{
    return reinterpret_cast<uintptr_t>(job->schedulerPrivate[1]);
}

// From a time on one of the runtime's clocks to one on CLOCK_MONOTONIC.
uint64_t DeadlineOn(int clock, long long seconds, long long nanoseconds)
{
    const SwiftTime now = swift_time_now(clock);
    const long long delay = (seconds - now.seconds) * static_cast<long long>(NanosecondsPerSecond) +
                            nanoseconds - now.nanoseconds;
    return Now() + (delay > 0 ? static_cast<uint64_t>(delay) : 0);
}

void MoveReadyDelayedJobs(JobQueue& queue)
{
    if (queue.delayed == nullptr)
    {
        return;
    }
    const uint64_t now = Now();
    while (queue.delayed != nullptr && DeadlineOf(queue.delayed) <= now)
    {
        SwiftJob* job = queue.delayed;
        queue.delayed = JobQueueTraits::getNext(job);
        queue.ready.enqueue(job);
    }
}

// Called and returns with queue_lock held. Without wait, returns null once nothing is queued.
SwiftJob* TakeJob(JobQueue& queue, bool wait)
{
    while (true)
    {
        MoveReadyDelayedJobs(queue);
        if (SwiftJob* job = queue.ready.dequeue())
        {
            return job;
        }
        if (queue.delayed != nullptr)
        {
            const uint64_t deadline = DeadlineOf(queue.delayed);
            const struct timespec until = {static_cast<time_t>(deadline / NanosecondsPerSecond),
                                           static_cast<long>(deadline % NanosecondsPerSecond)};
            pthread_cond_timedwait(&queue.queued, &queue_lock, &until);
        }
        else if (wait)
        {
            pthread_cond_wait(&queue.queued, &queue_lock);
        }
        else
        {
            return nullptr;
        }
    }
}

[[noreturn]] void* RunJobs(void*)
{
    pthread_mutex_lock(&queue_lock);
    while (true)
    {
        SwiftJob* job = TakeJob(global_queue, true);
        pthread_mutex_unlock(&queue_lock);
        swift_job_run(job, swift_executor_generic());
        pthread_mutex_lock(&queue_lock);
    }
}

void Start()
{
    pthread_condattr_t attributes;
    pthread_condattr_init(&attributes);
    pthread_condattr_setclock(&attributes, CLOCK_MONOTONIC);
    pthread_cond_init(&global_queue.queued, &attributes);
    pthread_cond_init(&main_queue.queued, &attributes);
    pthread_condattr_destroy(&attributes);

    const long cpus = sysconf(_SC_NPROCESSORS_ONLN);
    const long workers = cpus > 0 ? cpus : 1;
    for (long worker = 0; worker < workers; ++worker)
    {
        pthread_t thread = 0;
        if (pthread_create(&thread, nullptr, RunJobs, nullptr) != 0)
        {
            break;
        }
        pthread_detach(thread);
    }
}

void LockQueue()
{
    pthread_once(&started, Start);
    pthread_mutex_lock(&queue_lock);
}

void Enqueue(JobQueue& queue, SwiftJob* job)
{
    LockQueue();
    queue.ready.enqueue(job);
    pthread_cond_signal(&queue.queued);
    pthread_mutex_unlock(&queue_lock);
}

void EnqueueDelayed(JobQueue& queue, SwiftJob* job, uint64_t deadline)
{
    job->schedulerPrivate[1] = reinterpret_cast<void*>(static_cast<uintptr_t>(deadline));
    LockQueue();
    SwiftJob** position = &queue.delayed;
    while (*position != nullptr && DeadlineOf(*position) <= deadline)
    {
        position = reinterpret_cast<SwiftJob**>(&(*position)->schedulerPrivate[0]);
    }
    JobQueueTraits::setNext(job, *position);
    *position = job;
    // Waiting threads take their timeouts from the earliest deadline.
    pthread_cond_broadcast(&queue.queued);
    pthread_mutex_unlock(&queue_lock);
}

[[noreturn]] void RunMainJobs(SwiftExecutorRef executor)
{
    LockQueue();
    while (true)
    {
        SwiftJob* job = TakeJob(main_queue, true);
        pthread_mutex_unlock(&queue_lock);
        swift_job_run(job, executor);
        pthread_mutex_lock(&queue_lock);
    }
}

} // namespace

#if SWIFT_CONCURRENCY_EMBEDDED

namespace
{

// The main executor is an ordinary one to the runtime, which only compares its identity; its
// witness table is never called, since swift.patch sends its jobs to
// swift_task_enqueueMainExecutor.
alignas(16) char main_executor_identity[16];
alignas(16) char main_executor_witness_table[16];

SwiftExecutorRef MainExecutor()
{
    return swift_executor_ordinary(
        reinterpret_cast<SwiftHeapObject*>(main_executor_identity),
        reinterpret_cast<SwiftExecutorWitnessTable*>(main_executor_witness_table));
}

bool IsMain(SwiftExecutorRef executor)
{
    return swift_executor_getIdentity(executor) ==
           reinterpret_cast<SwiftHeapObject*>(main_executor_identity);
}

} // namespace

SWIFT_CC(swift) void swift_task_enqueueGlobalImpl(SwiftJob* job)
{
    Enqueue(global_queue, job);
}

SWIFT_CC(swift) void swift_task_enqueueMainExecutorImpl(SwiftJob* job)
{
    Enqueue(main_queue, job);
}

SWIFT_CC(swift) void swift_task_enqueueGlobalWithDelayImpl(SwiftJobDelay delay, SwiftJob* job)
{
    EnqueueDelayed(global_queue, job, Now() + delay);
}

SWIFT_CC(swift)
void swift_task_enqueueGlobalWithDeadlineImpl(long long seconds, long long nanoseconds, long long,
                                              long long, int clock, SwiftJob* job)
{
    EnqueueDelayed(global_queue, job, DeadlineOn(clock, seconds, nanoseconds));
}

SWIFT_CC(swift) void swift_task_checkIsolatedImpl(SwiftExecutorRef executor)
{
    if (!IsMain(executor))
    {
        swift_executor_invokeSwiftCheckIsolated(executor);
    }
    else if (!swift::Thread::onMainThread())
    {
        swift::swift_Concurrency_fatalError(0, "Expected to run on the main thread\n");
    }
}

SWIFT_CC(swift) int8_t swift_task_isIsolatingCurrentContextImpl(SwiftExecutorRef executor)
{
    if (IsMain(executor))
    {
        return swift::Thread::onMainThread() ? 1 : 0;
    }
    return swift_executor_invokeSwiftIsIsolatingCurrentContext(executor);
}

SWIFT_CC(swift)
void swift_task_donateThreadToGlobalExecutorUntilImpl(bool (*condition)(void*), void* context)
{
    while (!condition(context))
    {
        LockQueue();
        SwiftJob* job = TakeJob(global_queue, false);
        pthread_mutex_unlock(&queue_lock);
        if (job == nullptr)
        {
            return;
        }
        swift_job_run(job, swift_executor_generic());
    }
}

SWIFT_RUNTIME_ATTRIBUTE_NORETURN SWIFT_CC(swift) void swift_task_asyncMainDrainQueueImpl()
{
    RunMainJobs(MainExecutor());
}

SWIFT_CC(swift) SwiftExecutorRef swift_task_getMainExecutorImpl()
{
    return MainExecutor();
}

SWIFT_CC(swift) bool swift_task_isMainExecutorImpl(SwiftExecutorRef executor)
{
    return IsMain(executor);
}

#else

// The main executor is upstream's DispatchMainExecutor, whose reference the runtime gets from
// MainActor.

extern "C" SWIFT_CC(swift) void swift_dispatchEnqueueGlobal(SwiftJob* job)
{
    Enqueue(global_queue, job);
}

extern "C" SWIFT_CC(swift) void swift_dispatchEnqueueMain(SwiftJob* job)
{
    Enqueue(main_queue, job);
}

// A tolerance of -1 nanoseconds means none was given; the queues ignore tolerances.
extern "C" SWIFT_CC(swift) void swift_dispatchEnqueueWithDeadline(bool global, long long seconds,
                                                                  long long nanoseconds, long long,
                                                                  long long, int clock,
                                                                  SwiftJob* job)
{
    EnqueueDelayed(global ? global_queue : main_queue, job,
                   DeadlineOn(clock, seconds, nanoseconds));
}

extern "C" SWIFT_CC(swift) [[noreturn]] void swift_dispatchMain()
{
    RunMainJobs(swift_task_getMainExecutorImpl());
}

extern "C" SWIFT_CC(swift) void swift_dispatchAssertMainQueue()
{
    if (!swift::Thread::onMainThread())
    {
        swift::swift_Concurrency_fatalError(0, "Expected to run on the main thread\n");
    }
}

#endif
