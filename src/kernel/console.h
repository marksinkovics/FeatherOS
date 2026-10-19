#ifndef FEATHEROS_CONSOLE_H
#define FEATHEROS_CONSOLE_H

#include <stddef.h>

namespace FeatherOS
{
class KLogger;
class Term;
} // namespace FeatherOS

// The kernel console: serial plus the optional framebuffer terminal, with line editing for
// programs that read input. Requires the periodic timer to be running.
namespace FeatherOS::Console
{

void Initialize(KLogger* logger, Term* term);
void WriteCharacter(char character);
// Shows prompt, then echoes and edits input until Enter. Interrupts are enabled while waiting.
// Returns the line length; buffer receives at most capacity - 1 characters and a terminator.
size_t ReadLine(const char* prompt, char* buffer, size_t capacity);
// Updates the uptime display; ReadLine calls it while waiting.
void Service();

} // namespace FeatherOS::Console

#endif
