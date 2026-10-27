#include "pipe.h"

#include "kernel/memory/heap.h"
#include "kernel/scheduler.h"
#include "kernel/sync/spinlock.h"
#include "user/syscall_abi.h"

namespace FeatherOS::Pipes
{

struct Pipe
{
    Sync::SpinLock lock;
    // Readers wait here for data, and writers for space.
    Scheduler::WaitQueue readable;
    Scheduler::WaitQueue writable;
    uint8_t data[Capacity];
    uint64_t start;
    uint64_t length;
    uint32_t readers;
    uint32_t writers;
};

namespace
{

bool Interrupted(InterruptCheck interrupted)
{
    return interrupted != nullptr && interrupted();
}

} // namespace

Pipe* Create()
{
    auto* pipe = static_cast<Pipe*>(Memory::AllocateZeroed(1, sizeof(Pipe)));
    if (pipe == nullptr)
    {
        return nullptr;
    }
    pipe->readers = 1;
    pipe->writers = 1;
    return pipe;
}

void CloseEnd(Pipe* pipe, bool write_end)
{
    bool unused;
    {
        Sync::SpinLockGuard guard(pipe->lock);
        if (write_end)
        {
            --pipe->writers;
        }
        else
        {
            --pipe->readers;
        }
        unused = pipe->readers == 0 && pipe->writers == 0;
    }
    // Readers then see the end of the data, and writers a broken pipe.
    Scheduler::WakeAll(pipe->readable);
    Scheduler::WakeAll(pipe->writable);
    if (unused)
    {
        Memory::Deallocate(pipe);
    }
}

uint64_t Read(Pipe* pipe, void* buffer, uint64_t length, InterruptCheck interrupted)
{
    if (length == 0)
    {
        return 0;
    }
    const uint64_t flags = pipe->lock.LockIrqSave();
    while (pipe->length == 0 && pipe->writers > 0)
    {
        if (Interrupted(interrupted))
        {
            pipe->lock.UnlockIrqRestore(flags);
            return User::SyscallErrorInterrupted;
        }
        Scheduler::WaitOn(pipe->readable, pipe->lock);
    }
    const uint64_t count = length < pipe->length ? length : pipe->length;
    auto* destination = static_cast<uint8_t*>(buffer);
    for (uint64_t index = 0; index < count; ++index)
    {
        destination[index] = pipe->data[(pipe->start + index) % Capacity];
    }
    pipe->start = (pipe->start + count) % Capacity;
    pipe->length -= count;
    pipe->lock.UnlockIrqRestore(flags);
    if (count > 0)
    {
        Scheduler::WakeAll(pipe->writable);
    }
    return count;
}

uint64_t Write(Pipe* pipe, const void* buffer, uint64_t length, InterruptCheck interrupted)
{
    const auto* source = static_cast<const uint8_t*>(buffer);
    uint64_t total = 0;
    while (total < length)
    {
        const uint64_t piece = length - total < AtomicSize ? length - total : AtomicSize;
        const uint64_t flags = pipe->lock.LockIrqSave();
        while (pipe->readers > 0 && Capacity - pipe->length < piece)
        {
            if (Interrupted(interrupted))
            {
                pipe->lock.UnlockIrqRestore(flags);
                return total > 0 ? total : User::SyscallErrorInterrupted;
            }
            Scheduler::WaitOn(pipe->writable, pipe->lock);
        }
        if (pipe->readers == 0)
        {
            pipe->lock.UnlockIrqRestore(flags);
            return total > 0 ? total : User::SyscallErrorBrokenPipe;
        }
        for (uint64_t index = 0; index < piece; ++index)
        {
            pipe->data[(pipe->start + pipe->length + index) % Capacity] = source[total + index];
        }
        pipe->length += piece;
        total += piece;
        pipe->lock.UnlockIrqRestore(flags);
        Scheduler::WakeAll(pipe->readable);
    }
    return total;
}

uint64_t Buffered(Pipe* pipe)
{
    Sync::SpinLockGuard guard(pipe->lock);
    return pipe->length;
}

} // namespace FeatherOS::Pipes
