#ifndef FEATHEROS_TESTS_SCHEDULER_STUBS_H
#define FEATHEROS_TESTS_SCHEDULER_STUBS_H

#include <stdint.h>

#include <atomic>
#include <functional>
#include <vector>

// Stand-ins for the scheduler and timer that kernel code waits through, shared by the tests that
// link such code. Each WaitOn counts itself and runs on_wait with the caller's lock released, as
// another CPU would while the caller sleeps. Without on_wait, a thread sleeps until a WakeAll or 1 ms
// with the lock released, so tests can run real threads: the counters are atomic and current_task is per thread,
// as is Scheduler::CurrentLocal(). Settings and on_wait are written while one thread runs.
// WakeTask follows the real scheduler's token: a task not inside WaitOn keeps the wake-up, and its
// next WaitOn returns at once (counted in early_wakes) without releasing the caller's lock.
namespace TestScheduler
{

extern std::atomic<int> wait_count;
extern std::atomic<int> wake_count;
extern std::atomic<int> early_wakes;
extern std::atomic<uint64_t> last_wait_deadline;
extern std::atomic<bool> lock_held_during_wait;
extern std::atomic<int> volume_violations;
extern std::function<void()> on_wait;
extern std::vector<uint32_t> woken_tasks;
extern thread_local uint32_t current_task;
extern std::atomic<uint64_t> ticks;

void Reset();

} // namespace TestScheduler

#endif
