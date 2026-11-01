#ifndef FEATHEROS_CONSOLE_H
#define FEATHEROS_CONSOLE_H

#include <stddef.h>
#include <stdint.h>

namespace FeatherOS
{
class Term;
namespace Terminals
{
class Terminal;
} // namespace Terminals
} // namespace FeatherOS

// The kernel's virtual consoles, /dev/tty1 to /dev/tty4: each a terminal with its own framebuffer
// screen and status bar, of which one is shown and gets the keys. The first also writes to and
// reads from serial, and shows kernel messages.
namespace FeatherOS::Console
{

constexpr uint32_t Count = 4;

// The first console's. Usable before Initialize, writing to serial only.
Terminals::Terminal* Terminal();
// Console 0 to Count - 1, or null.
Terminals::Terminal* Terminal(uint32_t console);
// terms holds one screen per console, or is null without a framebuffer. Requires the periodic
// timer and the scheduler to be running.
void Initialize(Term* terms);
// Draws console on the framebuffer and sends it the keys; may run in interrupt context.
void Show(uint32_t console);
uint32_t Shown();
// Redraws the status bar soon, e.g. after the foreground program changes.
void NotifyStatusChanged();
// Kernel output, on the first console.
void WriteCharacter(char character);
void WriteLine(const char* text);
#if defined(FEATHEROS_INPUT_TEST)
// Serial only, kept whole among the first console's output.
void WriteSerialLine(const char* text);
#endif

} // namespace FeatherOS::Console

#endif
