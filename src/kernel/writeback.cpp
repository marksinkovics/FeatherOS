#include "writeback.h"

#include "kernel/files.h"
#include "kernel/klogger.h"
#include "kernel/panic.h"
#include "kernel/scheduler.h"
#include "kernel/time/timer.h"

#include <stdint.h>

namespace FeatherOS::Writeback
{
namespace
{

constinit KLogger logger;

uint64_t WritebackTask(void*)
{
    const uint64_t interval = uint64_t{IntervalSeconds} * Time::TicksPerSecond();
    uint64_t next = Time::TickCount() + interval;
    bool failing = false;
    for (;;)
    {
        while (Time::TickCount() < next)
        {
            Scheduler::SleepUntil(next);
        }
        next = Time::TickCount() + interval;
        const bool synced = Files::SyncPeriodic() == 0;
        if (!synced && !failing)
        {
            logger.Warning("Writeback: a mount did not sync");
        }
        failing = !synced;
    }
}

} // namespace

void Start()
{
    if (Time::TicksPerSecond() == 0)
    {
        return;
    }
    Scheduler::TaskId task;
    if (!Scheduler::CreateTask(WritebackTask, nullptr, &task) ||
        !Scheduler::SetPriority(task, Scheduler::TaskPriority::High))
    {
        Panic("Could not start the writeback task");
    }
}

} // namespace FeatherOS::Writeback
