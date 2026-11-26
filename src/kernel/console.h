#ifndef FEATHEROS_CONSOLE_H
#define FEATHEROS_CONSOLE_H

#include "user/syscall_abi.h"

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
class Framebuffer;

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
// terms holds one screen per console, drawing on framebuffer, or both are null without one.
// Requires the periodic timer and the scheduler to be running.
void Initialize(Term* terms, Framebuffer* framebuffer);
// Draws console on the framebuffer and sends it the keys; may run in interrupt context.
void Show(uint32_t console);
uint32_t Shown();
// Redraws the status bar soon, e.g. after the foreground program changes.
void NotifyStatusChanged();
// The console from 0 that terminal belongs to, or Count for a pseudo-terminal or null.
uint32_t IndexOf(const Terminals::Terminal* terminal);
// 0 by 0 without a framebuffer.
void DisplaySize(uint32_t* width, uint32_t* height);
// Graphics mode (/dev/fb0): owner, any token, takes console's screen, on which no text or status
// bar is drawn until ReleaseDisplay. SyscallErrorBusy if the console or the owner is already
// taken, except that the owner of the console may acquire it again; SyscallErrorInvalidArgument
// without a framebuffer. ReleaseDisplay fails with SyscallErrorInvalidArgument if owner holds
// none, and redraws the console whole.
uint64_t AcquireDisplay(uint32_t console, const void* owner, uint64_t flags);
uint64_t ReleaseDisplay(const void* owner);
bool OwnsDisplay(const void* owner);
// The owner's key events and Shown/Hidden, oldest first, while it owns a console; they are queued
// whether or not the console is shown. TakeEvents returns the count taken (at most capacity, 0 if
// none wait) or SyscallErrorInvalidArgument if owner holds none. WaitForEvents sleeps until events
// wait or the owner loses its console, and returns at once if either holds. PollEvents is
// PollReadable if events wait, PollError if owner holds none, else 0. DroppedEvents counts the
// events refused since the owner acquired.
uint64_t TakeEvents(const void* owner, User::FramebufferEvent* events, uint32_t capacity);
void WaitForEvents(const void* owner);
int16_t PollEvents(const void* owner);
uint32_t DroppedEvents(const void* owner);
// Converts count colors into screen row y from screen_x, and draws that row if owner's console is
// shown. False if owner holds no console, so nothing was written.
bool PresentRow(const void* owner, uint32_t screen_x, uint32_t y, const uint32_t* pixels,
                uint32_t count);
// Kernel output, on the first console.
void WriteCharacter(char character);
void WriteLine(const char* text);
#if defined(FEATHEROS_KERNEL_TESTS)
// Serial only, kept whole among the first console's output.
void WriteSerialLine(const char* text);
#endif

} // namespace FeatherOS::Console

#endif
