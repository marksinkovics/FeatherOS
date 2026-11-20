#ifndef FEATHEROS_FRAMEBUFFER_GRAPHICS_MODE_H
#define FEATHEROS_FRAMEBUFFER_GRAPHICS_MODE_H

#include "kernel/console.h"
#include "user/syscall_abi.h"

#include <stdint.h>

// The state behind a program taking over a console's screen, without locks or kernel services so
// that the host tests can run it. Console holds the lock that guards it.
namespace FeatherOS::GraphicsMode
{

// The visible part of a present's rectangle, and where its first pixel is in the program's
// memory, in pixels from the request's pixels.
struct Clipped
{
    uint32_t screen_x;
    uint32_t screen_y;
    uint32_t width;
    uint32_t height;
    uint64_t source_offset;
};

// 0, with a width and height of 0 when nothing is visible; SyscallErrorInvalidArgument for
// flags, or a stride, width or height past User::MaximumFramebufferDimension, or a stride below
// the width of a rectangle that is not empty. An empty rectangle is never read.
uint64_t Clip(const User::FramebufferPresentRequest& request, uint32_t screen_width,
              uint32_t screen_height, Clipped* clipped);

// One owner per console and one console per owner; an owner is any token, the open file.
class Owners
{
public:
    // 0, also if owner already holds console; SyscallErrorBusy if console has another owner or
    // owner holds another console, SyscallErrorInvalidArgument for a console that does not exist.
    uint64_t Acquire(uint32_t console, const void* owner);
    // The console owner held, or Console::Count if it held none.
    uint32_t Release(const void* owner);
    bool Owns(uint32_t console, const void* owner) const;
    bool Acquired(uint32_t console) const;
    // The console owner holds, or Console::Count.
    uint32_t ConsoleOf(const void* owner) const;

private:
    const void* m_owners[Console::Count] = {};
};

} // namespace FeatherOS::GraphicsMode

#endif
