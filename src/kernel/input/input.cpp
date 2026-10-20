#include "input.h"

#include "kernel/arch/serial.h"
#include "kernel/scheduler.h"
#include "kernel/sync/spinlock.h"

namespace FeatherOS::Input
{
namespace
{

constexpr uint32_t QueueCapacity = 64;

Sync::SpinLock queue_lock;
Scheduler::WaitQueue event_waiters;
Event events[QueueCapacity];
uint32_t queue_head = 0;
uint32_t queue_tail = 0;
uint32_t queue_size = 0;
bool previous_was_carriage_return = false;
InterruptHandler interrupt_handler = nullptr;
uint32_t pending_interrupts = 0;

void SubmitEventLocked(Event event)
{
    if (queue_size < QueueCapacity)
    {
        events[queue_tail] = event;
        queue_tail = (queue_tail + 1) % QueueCapacity;
        ++queue_size;
    }
}

void ProcessCharacterLocked(char character)
{
    if (character == '\n' && previous_was_carriage_return)
    {
        previous_was_carriage_return = false;
        return;
    }
    previous_was_carriage_return = character == '\r';

    if (character == '\r' || character == '\n')
    {
        SubmitEventLocked({EventType::Enter, '\0'});
    }
    else if (character == '\b' || character == '\x7f')
    {
        SubmitEventLocked({EventType::Backspace, '\0'});
    }
    else if (character == '\x03')
    {
        ++pending_interrupts;
    }
    else if (character >= 0x20 && character <= 0x7e)
    {
        SubmitEventLocked({EventType::Character, character});
    }
}

void DeliverInterrupt()
{
    if (interrupt_handler != nullptr && interrupt_handler())
    {
        return;
    }
    Sync::SpinLockGuard guard(queue_lock);
    SubmitEventLocked({EventType::Interrupt, '\0'});
    Scheduler::WakeAll(event_waiters);
}

} // namespace

void SetInterruptHandler(InterruptHandler handler)
{
    interrupt_handler = handler;
}

void SubmitEvent(Event event)
{
    if (event.type == EventType::Interrupt)
    {
        DeliverInterrupt();
        return;
    }
    Sync::SpinLockGuard guard(queue_lock);
    SubmitEventLocked(event);
    Scheduler::WakeAll(event_waiters);
}

void Poll()
{
    uint32_t interrupts;
    {
        Sync::SpinLockGuard guard(queue_lock);
        const uint32_t previous_size = queue_size;
        char character;
        while (Serial::ReadCharacter(&character))
        {
            ProcessCharacterLocked(character);
        }
        if (queue_size != previous_size)
        {
            Scheduler::WakeAll(event_waiters);
        }
        interrupts = pending_interrupts;
        pending_interrupts = 0;
    }
    // The handler takes other subsystems' locks, so it runs after the queue lock is released.
    for (; interrupts > 0; --interrupts)
    {
        DeliverInterrupt();
    }
}

bool TryReadEvent(Event* event)
{
    if (event == nullptr)
    {
        return false;
    }

    Sync::SpinLockGuard guard(queue_lock);
    if (queue_size == 0)
    {
        return false;
    }
    *event = events[queue_head];
    queue_head = (queue_head + 1) % QueueCapacity;
    --queue_size;
    return true;
}

void WaitForEvent(uint64_t deadline_tick)
{
    const uint64_t flags = queue_lock.LockIrqSave();
    if (queue_size == 0)
    {
        Scheduler::WaitOn(event_waiters, queue_lock, deadline_tick);
    }
    queue_lock.UnlockIrqRestore(flags);
}

} // namespace FeatherOS::Input