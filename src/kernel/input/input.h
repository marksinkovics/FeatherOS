#ifndef FEATHEROS_INPUT_H
#define FEATHEROS_INPUT_H

#include <stdint.h>

namespace FeatherOS::Input
{

enum class EventType : uint8_t
{
    Character,
    Backspace,
    Enter,
    // Ctrl+C.
    Interrupt,
};

struct Event
{
    EventType type;
    char character;
};

// Returns true when it consumed the Ctrl+C; otherwise the Interrupt event is queued for the reader.
// It may run in interrupt context, outside the queue lock.
using InterruptHandler = bool (*)();

void SetInterruptHandler(InterruptHandler handler);
void SubmitEvent(Event event);
void Poll();
bool TryReadEvent(Event* event);
void WaitForEvent(uint64_t deadline_tick);

} // namespace FeatherOS::Input

#endif