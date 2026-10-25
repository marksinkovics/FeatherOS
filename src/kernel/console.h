#ifndef FEATHEROS_CONSOLE_H
#define FEATHEROS_CONSOLE_H

#include "user/syscall_abi.h"

#include <stddef.h>
#include <stdint.h>

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
// Prints printable ASCII and newlines as one unit, so other output cannot interleave with it.
// Escape sequences, \r and \b pass through to the terminal and serial. Tab prints as a space,
// bytes above 0x7F as '?', and other control bytes are dropped.
void Write(const char* text, size_t length);
// The text area below the status bar, or a serial terminal's usual 24 by 80 without a screen.
void Size(uint32_t* rows, uint32_t* columns);
// Shows prompt, then edits input until Enter, echoing it unless the settings turn echo off.
// Interrupts are enabled while waiting. buffer receives at most capacity - 1 characters and a
// terminator. Returns false when a signal interrupted the calling program.
bool ReadLine(const char* prompt, char* buffer, size_t capacity, size_t* length);
// A non-canonical read, as the settings' minimum and timeout describe. Returns false when a signal
// interrupted the calling program before any byte arrived.
bool ReadBytes(char* buffer, size_t capacity, size_t* length);

User::TerminalSettings Settings();
bool IsDefault(const User::TerminalSettings& settings);
// owner keeps settings other than the defaults until it sets the defaults or ReleaseSettings
// restores them, together with the terminal's cursor and video modes.
void SetSettings(const User::TerminalSettings& settings, uint64_t owner);
void ReleaseSettings(uint64_t owner);

} // namespace FeatherOS::Console

#endif
