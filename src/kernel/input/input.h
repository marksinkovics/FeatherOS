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
};

struct Event
{
    EventType type;
    char character;
};

void SubmitEvent(Event event);
void Poll();
bool TryReadEvent(Event* event);

} // namespace FeatherOS::Input

#endif