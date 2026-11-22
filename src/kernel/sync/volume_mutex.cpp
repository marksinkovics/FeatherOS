#include "kernel/sync/mutex.h"

namespace FeatherOS::Sync
{

void VolumeMutex::Acquire()
{
    const uint64_t task = static_cast<uint64_t>(Scheduler::CurrentTaskId()) + 1;
    const uint64_t flags = state_lock.LockIrqSave();
    while (owner.load(std::memory_order_relaxed) != 0)
    {
        Scheduler::WaitOn(waiters, state_lock);
    }
    owner.store(task, std::memory_order_relaxed);
    state_lock.UnlockIrqRestore(flags);
}

void VolumeMutex::Lock()
{
    Scheduler::TaskLocal& local = Scheduler::CurrentLocal();
    if (local.held_volume != nullptr)
    {
        VolumeLockViolation();
    }
    Acquire();
    local.held_volume = this;
}

void VolumeMutex::LockWithOthers()
{
    Acquire();
}

void VolumeMutex::Unlock()
{
    Scheduler::TaskLocal& local = Scheduler::CurrentLocal();
    if (local.held_volume == this)
    {
        local.held_volume = nullptr;
    }
    const uint64_t flags = state_lock.LockIrqSave();
    owner.store(0, std::memory_order_relaxed);
    Scheduler::WakeAll(waiters);
    state_lock.UnlockIrqRestore(flags);
}

bool VolumeMutex::HeldByCurrentTask() const
{
    return owner.load(std::memory_order_relaxed) ==
           static_cast<uint64_t>(Scheduler::CurrentTaskId()) + 1;
}

VolumeHoldBase::VolumeHoldBase(VolumeMutex* mutex)
    : mutex(mutex), locked_here(mutex != nullptr && !mutex->HeldByCurrentTask())
{
    if (locked_here)
    {
        mutex->Lock();
    }
}

VolumeHoldBase::VolumeHoldBase(VolumeMutex& mutex) : VolumeHoldBase(&mutex) {}

VolumeHoldBase::~VolumeHoldBase()
{
    if (locked_here)
    {
        mutex->Unlock();
    }
}

} // namespace FeatherOS::Sync
