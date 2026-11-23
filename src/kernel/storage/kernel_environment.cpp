#include "kernel_environment.h"

#include "kernel/scheduler.h"
#include "kernel/sync/spinlock.h"
#include "kernel/time/timer.h"
#include "user/thread_safety.h"

#include <stddef.h>
#include <stdint.h>

namespace FeatherOS::Storage
{
namespace
{

// Channels share a few queues; a wake-up meant for another channel is only a spurious one.
constexpr size_t QueueCount = 8;

constinit Sync::SpinLock wait_lock;
Scheduler::WaitQueue queues[QueueCount];

Scheduler::WaitQueue& QueueOf(const void* channel)
{
    return queues[(reinterpret_cast<uintptr_t>(channel) >> 4) % QueueCount];
}

uint64_t TickAt(uint64_t milliseconds)
{
    const uint64_t tick = (milliseconds * Time::TicksPerSecond() + 999) / 1000;
    return tick == 0 ? 1 : tick;
}

class Kernel final : public Environment
{
public:
    uint64_t NowMilliseconds() override
    {
        return Time::TickCount() * 1000 / Time::TicksPerSecond();
    }

    bool WaitUntil(const void* channel, uint64_t deadline_ms, bool (*ready)(void*),
                   void* context) override
    {
        Sync::SpinLockGuard guard(wait_lock);
        while (!ready(context))
        {
            if (NowMilliseconds() >= deadline_ms)
            {
                return false;
            }
            Scheduler::WaitOn(QueueOf(channel), wait_lock, TickAt(deadline_ms));
        }
        return true;
    }

    void Wake(const void* channel) override
    {
        Sync::SpinLockGuard guard(wait_lock);
        Scheduler::WakeAll(QueueOf(channel));
    }

    void Sleep(uint32_t milliseconds) override
    {
        Scheduler::SleepUntil(TickAt(NowMilliseconds() + milliseconds));
    }
};

constinit Kernel kernel;

} // namespace

Environment& KernelEnvironment()
{
    return kernel;
}

} // namespace FeatherOS::Storage
