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

// The global executor, in place of upstream's single-threaded CooperativeGlobalExecutor.cpp: one
// queue of jobs, run by a worker thread for every CPU, which start with the first job. MainActor's
// jobs wait in a queue of their own, which only the main thread runs, in
// swift_task_asyncMainDrainQueue.

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

pthread_once_t started = PTHREAD_ONCE_INIT;
pthread_mutex_t queue_lock = PTHREAD_MUTEX_INITIALIZER;
// On CLOCK_MONOTONIC, set up by Start.
pthread_cond_t job_queued;
swift::PriorityQueue<SwiftJob*, JobQueueTraits> ready_jobs;
pthread_cond_t main_job_queued = PTHREAD_COND_INITIALIZER;
swift::PriorityQueue<SwiftJob*, JobQueueTraits> main_jobs;
// Sorted by deadline, then in the order they came.
SwiftJob* delayed_jobs = nullptr;

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

void MoveReadyDelayedJobs()
{
    if (delayed_jobs == nullptr)
    {
        return;
    }
    const uint64_t now = Now();
    while (delayed_jobs != nullptr && DeadlineOf(delayed_jobs) <= now)
    {
        SwiftJob* job = delayed_jobs;
        delayed_jobs = JobQueueTraits::getNext(job);
        ready_jobs.enqueue(job);
    }
}

// Called and returns with queue_lock held. Without wait, returns null once nothing is queued.
SwiftJob* TakeJob(bool wait)
{
    while (true)
    {
        MoveReadyDelayedJobs();
        if (SwiftJob* job = ready_jobs.dequeue())
        {
            return job;
        }
        if (delayed_jobs != nullptr)
        {
            const uint64_t deadline = DeadlineOf(delayed_jobs);
            const struct timespec until = {static_cast<time_t>(deadline / NanosecondsPerSecond),
                                           static_cast<long>(deadline % NanosecondsPerSecond)};
            pthread_cond_timedwait(&job_queued, &queue_lock, &until);
        }
        else if (wait)
        {
            pthread_cond_wait(&job_queued, &queue_lock);
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
        SwiftJob* job = TakeJob(true);
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
    pthread_cond_init(&job_queued, &attributes);
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

void EnqueueDelayed(SwiftJob* job, uint64_t deadline)
{
    job->schedulerPrivate[1] = reinterpret_cast<void*>(static_cast<uintptr_t>(deadline));
    LockQueue();
    SwiftJob** position = &delayed_jobs;
    while (*position != nullptr && DeadlineOf(*position) <= deadline)
    {
        position = reinterpret_cast<SwiftJob**>(&(*position)->schedulerPrivate[0]);
    }
    JobQueueTraits::setNext(job, *position);
    *position = job;
    // Waiting threads take their timeouts from the earliest deadline.
    pthread_cond_broadcast(&job_queued);
    pthread_mutex_unlock(&queue_lock);
}

} // namespace

SWIFT_CC(swift) void swift_task_enqueueGlobalImpl(SwiftJob* job)
{
    LockQueue();
    ready_jobs.enqueue(job);
    pthread_cond_signal(&job_queued);
    pthread_mutex_unlock(&queue_lock);
}

SWIFT_CC(swift) void swift_task_enqueueMainExecutorImpl(SwiftJob* job)
{
    LockQueue();
    main_jobs.enqueue(job);
    pthread_cond_signal(&main_job_queued);
    pthread_mutex_unlock(&queue_lock);
}

SWIFT_CC(swift) void swift_task_enqueueGlobalWithDelayImpl(SwiftJobDelay delay, SwiftJob* job)
{
    EnqueueDelayed(job, Now() + delay);
}

SWIFT_CC(swift)
void swift_task_enqueueGlobalWithDeadlineImpl(long long seconds, long long nanoseconds, long long,
                                              long long, int clock, SwiftJob* job)
{
    const SwiftTime now = swift_time_now(clock);
    const long long delay = (seconds - now.seconds) * static_cast<long long>(NanosecondsPerSecond) +
                            nanoseconds - now.nanoseconds;
    EnqueueDelayed(job, Now() + (delay > 0 ? static_cast<uint64_t>(delay) : 0));
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
        SwiftJob* job = TakeJob(false);
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
    LockQueue();
    while (true)
    {
        SwiftJob* job = main_jobs.dequeue();
        if (job == nullptr)
        {
            pthread_cond_wait(&main_job_queued, &queue_lock);
            continue;
        }
        pthread_mutex_unlock(&queue_lock);
        swift_job_run(job, MainExecutor());
        pthread_mutex_lock(&queue_lock);
    }
}

SWIFT_CC(swift) SwiftExecutorRef swift_task_getMainExecutorImpl()
{
    return MainExecutor();
}

SWIFT_CC(swift) bool swift_task_isMainExecutorImpl(SwiftExecutorRef executor)
{
    return IsMain(executor);
}
