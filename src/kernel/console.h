#ifndef FEATHEROS_CONSOLE_H
#define FEATHEROS_CONSOLE_H

#include <stddef.h>

namespace FeatherOS
{
class Term;
} // namespace FeatherOS

// The kernel console: serial plus the optional framebuffer terminal, with line editing for
// programs that read input. Requires the periodic timer and the scheduler to be running.
namespace FeatherOS::Console
{

void Initialize(Term* term);
// Redraws the status bar soon, e.g. after the foreground program changes.
void NotifyStatusChanged();
void WriteCharacter(char character);
void WriteLine(const char* text);
// Shows prompt, then echoes and edits input until Enter. Interrupts are enabled while waiting.
// buffer receives at most capacity - 1 characters and a terminator. Returns false when Ctrl+C
// interrupted the calling program.
bool ReadLine(const char* prompt, char* buffer, size_t capacity, size_t* length);

} // namespace FeatherOS::Console

#endif
