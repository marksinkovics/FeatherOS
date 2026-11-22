#include "pointer.h"

#include "kernel/poll.h"
#include "kernel/scheduler.h"
#include "kernel/string/memory_functions.h"
#include "kernel/sync/spinlock.h"
#include "kernel/time/timer.h"
#include "user/thread_safety.h"

namespace FeatherOS::Input::Pointer
{
namespace
{

constexpr uint64_t MicrosecondsPerSecond = 1'000'000;

constinit Sync::SpinLock pointer_lock;
constinit Scheduler::WaitQueue readable;
User::InputEvent ring[RingSize] GUARDED_BY(pointer_lock) = {};
// Sequence numbers: the next record to be written, and the first record of the oldest report
// still in the ring. A report is never partly in the ring.
uint64_t end GUARDED_BY(pointer_lock) = 0;
uint64_t start GUARDED_BY(pointer_lock) = 0;

void Stamp(User::InputEvent* event)
{
    const uint64_t ticks = Time::TickCount();
    const uint64_t rate = Time::TicksPerSecond();
    event->seconds = static_cast<int64_t>(ticks / rate);
    event->microseconds = static_cast<int64_t>((ticks % rate) * MicrosecondsPerSecond / rate);
}

void DropOldestReportLocked() REQUIRES(pointer_lock)
{
    while (start < end)
    {
        const User::InputEvent& event = ring[start % RingSize];
        ++start;
        if (event.type == User::InputEventSync && event.code == User::InputSyncReport)
        {
            return;
        }
    }
}

} // namespace

void Submit(const User::InputEvent* events, size_t count)
{
    if (count == 0 || count > RingSize || events[count - 1].type != User::InputEventSync ||
        events[count - 1].code != User::InputSyncReport)
    {
        return;
    }
    {
        Sync::SpinLockGuard guard(pointer_lock);
        // Before the new report overwrites the old records the search for a report end reads.
        while (end + count - start > RingSize)
        {
            DropOldestReportLocked();
        }
        for (size_t index = 0; index < count; ++index)
        {
            User::InputEvent& slot = ring[(end + index) % RingSize];
            slot = events[index];
            Stamp(&slot);
        }
        end += count;
    }
    Scheduler::WakeAll(readable);
    Poll::Notify();
}

Reader Open()
{
    Sync::SpinLockGuard guard(pointer_lock);
    return Reader{end};
}

uint64_t Read(Reader& reader, void* buffer, uint64_t bytes, bool nonblocking,
              Pipes::InterruptCheck interrupted)
{
    if (bytes < sizeof(User::InputEvent))
    {
        return User::SyscallErrorInvalidArgument;
    }
    const uint64_t capacity = bytes / sizeof(User::InputEvent);
    auto* destination = static_cast<uint8_t*>(buffer);
    const uint64_t flags = pointer_lock.LockIrqSave();
    while (reader.next == end)
    {
        if (nonblocking || (interrupted != nullptr && interrupted()))
        {
            pointer_lock.UnlockIrqRestore(flags);
            return nonblocking ? User::SyscallErrorWouldBlock : User::SyscallErrorInterrupted;
        }
        Scheduler::WaitOn(readable, pointer_lock);
    }
    uint64_t count = 0;
    if (reader.next < start)
    {
        User::InputEvent dropped = {};
        dropped.type = User::InputEventSync;
        dropped.code = User::InputSyncDropped;
        Stamp(&dropped);
        memcpy(destination, &dropped, sizeof(dropped));
        reader.next = start;
        count = 1;
    }
    while (count < capacity && reader.next < end)
    {
        memcpy(destination + count * sizeof(User::InputEvent), &ring[reader.next % RingSize],
               sizeof(User::InputEvent));
        ++reader.next;
        ++count;
    }
    pointer_lock.UnlockIrqRestore(flags);
    return count * sizeof(User::InputEvent);
}

int16_t PollEvents(const Reader& reader)
{
    Sync::SpinLockGuard guard(pointer_lock);
    return reader.next != end ? User::PollReadable : 0;
}

} // namespace FeatherOS::Input::Pointer
