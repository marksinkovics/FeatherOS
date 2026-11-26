#ifndef FEATHEROS_FRAMEBUFFER_GRAPHICS_MODE_H
#define FEATHEROS_FRAMEBUFFER_GRAPHICS_MODE_H

#include "kernel/console.h"
#include "kernel/input/input.h"
#include "kernel/input/keyboard.h"
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

// Two slots stay free for Shown and Hidden, which key events never take.
constexpr uint32_t KeyEventLimit = User::FramebufferEventCapacity - 2;

static_assert(Input::Modifier::Shift == User::FramebufferModifierShift &&
                  Input::Modifier::Control == User::FramebufferModifierControl &&
                  Input::Modifier::Alt == User::FramebufferModifierAlt &&
                  Input::Modifier::CapsLock == User::FramebufferModifierCapsLock &&
                  Input::Modifier::Super == User::FramebufferModifierSuper,
              "the kernel and the C library share the modifier bits");

// A fixed ring that never overwrites: a push that does not fit is dropped and counted.
class EventQueue
{
public:
    // False, and one more dropped, when limit events are queued already.
    bool Push(const User::FramebufferEvent& event, uint32_t limit);
    uint32_t Pop(User::FramebufferEvent* events, uint32_t capacity);
    uint32_t Size() const;
    uint32_t Dropped() const;
    void Clear();

private:
    User::FramebufferEvent m_events[User::FramebufferEventCapacity] = {};
    uint32_t m_head = 0;
    uint32_t m_size = 0;
    uint32_t m_dropped = 0;
};

// One owner per console and one console per owner; an owner is any token, the open file. Each
// owner has a queue of events (key presses, repeats and releases, then Shown and Hidden) that
// stays well formed per key: a key's events are Press, Repeat* and Release.
class Owners
{
public:
    // 0, also if owner already holds console; SyscallErrorBusy if console has another owner or
    // owner holds another console, SyscallErrorInvalidArgument for a console that does not exist.
    // A new owner's first event is Shown if shown, else Hidden.
    uint64_t Acquire(uint32_t console, const void* owner, bool shown, uint32_t time_ms);
    // The console owner held, or Console::Count if it held none. Discards its events.
    uint32_t Release(const void* owner);
    bool Owns(uint32_t console, const void* owner) const;
    bool Acquired(uint32_t console) const;
    // The console owner holds, or Console::Count.
    uint32_t ConsoleOf(const void* owner) const;

    // False if console has no owner, it is hidden, or the code is not tracked; true if the key was
    // queued or dropped (counted).
    bool DeliverKey(uint32_t console, const Input::KeyEvent& event, uint32_t time_ms);
    // Queues a Release for every key the owner saw pressed, then Hidden. Nothing without an owner
    // or if already hidden.
    void Hide(uint32_t console, uint8_t modifiers, uint32_t time_ms);
    void Show(uint32_t console, uint32_t time_ms);
    // Up to capacity events, oldest first; SyscallErrorInvalidArgument if owner holds no console.
    uint64_t TakeEvents(const void* owner, User::FramebufferEvent* events, uint32_t capacity);
    uint32_t PendingEvents(const void* owner) const;
    uint32_t DroppedEvents(const void* owner) const;

private:
    struct Slot
    {
        const void* owner = nullptr;
        bool shown = false;
        EventQueue queue;
        // Keys whose Press the owner has been given and whose Release it has not.
        uint64_t seen[Input::Keyboard::TrackedKeys / 64] = {};
    };

    const Slot* SlotOf(const void* owner) const;
    Slot* SlotOf(const void* owner);

    Slot m_slots[Console::Count] = {};
};

} // namespace FeatherOS::GraphicsMode

#endif
