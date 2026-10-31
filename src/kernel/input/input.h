#ifndef FEATHEROS_INPUT_H
#define FEATHEROS_INPUT_H

#include <stddef.h>
#include <stdint.h>

// One queue of input bytes from the keyboards and the serial port, as a terminal sends them (see
// Keyboard). A serial "\r\n" counts as one '\r'.
namespace FeatherOS::Input
{

constexpr char InterruptByte = '\x03';

// Returns true when it consumed the Ctrl+C; otherwise the byte is queued for the reader.
// It may run in interrupt context, outside the queue lock.
using InterruptHandler = bool (*)();

void SetInterruptHandler(InterruptHandler handler);
// A key's bytes are queued whole or, when the queue is full, not at all.
void SubmitBytes(const char* bytes, size_t length);
void Poll();
bool TryReadByte(char* byte);
// Polls the serial port first, so its bytes count too.
bool HasInput();
void WaitForInput(uint64_t deadline_tick);
// Wakes WaitForInput's callers early, which must then check what they wait for themselves.
void WakeWaiters();

} // namespace FeatherOS::Input

#endif
