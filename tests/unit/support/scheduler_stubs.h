#ifndef FEATHEROS_TESTS_SCHEDULER_STUBS_H
#define FEATHEROS_TESTS_SCHEDULER_STUBS_H

#include <stdint.h>

#include <functional>
#include <vector>

// Stand-ins for the scheduler and timer that kernel code waits through, shared by the tests that
// link such code. Each WaitOn counts itself and runs on_wait with the caller's lock released, as
// another CPU would while the caller sleeps.
namespace TestScheduler
{

extern int wait_count;
extern int wake_count;
extern uint64_t last_wait_deadline;
extern bool lock_held_during_wait;
extern std::function<void()> on_wait;
extern std::vector<uint32_t> woken_tasks;
extern uint32_t current_task;
extern uint64_t ticks;

void Reset();

} // namespace TestScheduler

#endif
