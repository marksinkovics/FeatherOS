#ifndef FEATHEROS_INPUT_H
#define FEATHEROS_INPUT_H

#include <stddef.h>
#include <stdint.h>

namespace FeatherOS::Terminals
{
class Terminal;
} // namespace FeatherOS::Terminals

namespace FeatherOS::Input
{
class Keyboard;
} // namespace FeatherOS::Input

// The consoles' input: bytes from the serial port go to the first console's terminal, and keys to
// the terminal of the console that is shown, as a terminal sends them (see Keyboard). A serial
// "\r\n" counts as one '\r'.
namespace FeatherOS::Input
{

// Both the serial and the keyboard terminal.
void SetTerminal(Terminals::Terminal* terminal);
void SetKeyboardTerminal(Terminals::Terminal* terminal);
// Called for Alt+F1 to Alt+F4 with console 0 to 3, perhaps in interrupt context.
void SetConsoleSwitch(void (*switch_console)(uint32_t console));
// A key event from a keyboard driver.
void SubmitKey(Keyboard& keyboard, uint16_t code, bool pressed);
// A key's bytes, passed on together.
void SubmitBytes(const char* bytes, size_t length);
// Reads what the serial port received.
void Poll();

} // namespace FeatherOS::Input

#endif
