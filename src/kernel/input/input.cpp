#include "input.h"

#include "kernel/arch/serial.h"
#include "kernel/scheduler.h"
#include "kernel/sync/spinlock.h"

namespace FeatherOS::Input
{
namespace
{

constexpr uint32_t QueueCapacity = 256;

constinit Sync::SpinLock queue_lock;
// One caller reads the serial port at a time and keeps it while delivering a Ctrl+C, so bytes
// typed after the Ctrl+C cannot be queued before it from another CPU. A timer tick that finds it
// taken, perhaps by the code it interrupted, skips its poll.
constinit Sync::SpinLock poll_lock;
constinit Scheduler::WaitQueue input_waiters;
char queue[QueueCapacity];
uint32_t queue_head = 0;
uint32_t queue_tail = 0;
uint32_t queue_size = 0;
bool previous_was_carriage_return = false;
InterruptHandler interrupt_handler = nullptr;

void QueueByteLocked(char byte)
{
    queue[queue_tail] = byte;
    queue_tail = (queue_tail + 1) % QueueCapacity;
    ++queue_size;
}

// Returns true for Ctrl+C, which the caller delivers before reading further bytes.
bool ProcessSerialByteLocked(char byte)
{
    const bool after_carriage_return = previous_was_carriage_return;
    previous_was_carriage_return = byte == '\r';
    if (byte == InterruptByte)
    {
        return true;
    }
    if ((byte != '\n' || !after_carriage_return) && queue_size < QueueCapacity)
    {
        QueueByteLocked(byte);
    }
    return false;
}

void DeliverInterrupt()
{
    if (interrupt_handler != nullptr && interrupt_handler())
    {
        return;
    }
    Sync::SpinLockGuard guard(queue_lock);
    if (queue_size < QueueCapacity)
    {
        QueueByteLocked(InterruptByte);
    }
    Scheduler::WakeAll(input_waiters);
}

} // namespace

void SetInterruptHandler(InterruptHandler handler)
{
    interrupt_handler = handler;
}

void SubmitBytes(const char* bytes, size_t length)
{
    if (length == 1 && bytes[0] == InterruptByte)
    {
        DeliverInterrupt();
        return;
    }
    Sync::SpinLockGuard guard(queue_lock);
    if (length > QueueCapacity - queue_size)
    {
        return;
    }
    for (size_t index = 0; index < length; ++index)
    {
        QueueByteLocked(bytes[index]);
    }
    Scheduler::WakeAll(input_waiters);
}

void Poll()
{
    if (!poll_lock.TryLock())
    {
        return;
    }
    for (;;)
    {
        bool interrupt = false;
        {
            Sync::SpinLockGuard guard(queue_lock);
            const uint32_t previous_size = queue_size;
            char byte;
            while (!interrupt && Serial::ReadCharacter(&byte))
            {
                interrupt = ProcessSerialByteLocked(byte);
            }
            if (queue_size != previous_size)
            {
                Scheduler::WakeAll(input_waiters);
            }
        }
        if (!interrupt)
        {
            break;
        }
        // The handler takes other subsystems' locks, so it runs after the queue lock is released.
        DeliverInterrupt();
    }
    poll_lock.Unlock();
}

bool TryReadByte(char* byte)
{
    if (byte == nullptr)
    {
        return false;
    }
    Sync::SpinLockGuard guard(queue_lock);
    if (queue_size == 0)
    {
        return false;
    }
    *byte = queue[queue_head];
    queue_head = (queue_head + 1) % QueueCapacity;
    --queue_size;
    return true;
}

void WaitForInput(uint64_t deadline_tick)
{
    const uint64_t flags = queue_lock.LockIrqSave();
    if (queue_size == 0)
    {
        Scheduler::WaitOn(input_waiters, queue_lock, deadline_tick);
    }
    queue_lock.UnlockIrqRestore(flags);
}

} // namespace FeatherOS::Input
