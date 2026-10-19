#include "input.h"

#include "kernel/arch/interrupts.h"
#include "kernel/arch/serial.h"

namespace FeatherOS::Input
{
namespace
{

constexpr uint32_t QueueCapacity = 64;

Event events[QueueCapacity];
uint32_t queue_head = 0;
uint32_t queue_tail = 0;
uint32_t queue_size = 0;
bool previous_was_carriage_return = false;

void ProcessCharacter(char character)
{
    if (character == '\n' && previous_was_carriage_return)
    {
        previous_was_carriage_return = false;
        return;
    }
    previous_was_carriage_return = character == '\r';

    if (character == '\r' || character == '\n')
    {
        SubmitEvent({EventType::Enter, '\0'});
    }
    else if (character == '\b' || character == '\x7f')
    {
        SubmitEvent({EventType::Backspace, '\0'});
    }
    else if (character >= 0x20 && character <= 0x7e)
    {
        SubmitEvent({EventType::Character, character});
    }
}

} // namespace

void SubmitEvent(Event event)
{
    const uint64_t flags = Arch::Interrupts::SaveAndDisable();
    if (queue_size < QueueCapacity)
    {
        events[queue_tail] = event;
        queue_tail = (queue_tail + 1) % QueueCapacity;
        ++queue_size;
    }
    Arch::Interrupts::Restore(flags);
}

void Poll()
{
    char character;
    while (Serial::ReadCharacter(&character))
    {
        ProcessCharacter(character);
    }
}

bool TryReadEvent(Event* event)
{
    if (event == nullptr)
    {
        return false;
    }

    const uint64_t flags = Arch::Interrupts::SaveAndDisable();
    if (queue_size == 0)
    {
        Arch::Interrupts::Restore(flags);
        return false;
    }
    *event = events[queue_head];
    queue_head = (queue_head + 1) % QueueCapacity;
    --queue_size;
    Arch::Interrupts::Restore(flags);
    return true;
}

} // namespace FeatherOS::Input