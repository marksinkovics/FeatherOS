#include "graphics_mode.h"

namespace FeatherOS::GraphicsMode
{

uint64_t Clip(const User::FramebufferPresentRequest& request, uint32_t screen_width,
              uint32_t screen_height, Clipped* clipped)
{
    *clipped = {};
    if (request.flags != 0 || request.stride > User::MaximumFramebufferDimension ||
        request.width > User::MaximumFramebufferDimension ||
        request.height > User::MaximumFramebufferDimension)
    {
        return User::SyscallErrorInvalidArgument;
    }
    if (request.width == 0 || request.height == 0)
    {
        return 0;
    }
    if (request.stride < request.width)
    {
        return User::SyscallErrorInvalidArgument;
    }
    const int64_t x = request.x;
    const int64_t y = request.y;
    int64_t left = x;
    if (left < 0)
    {
        left = 0;
    }
    int64_t top = y;
    if (top < 0)
    {
        top = 0;
    }
    int64_t right = x + request.width;
    if (right > screen_width)
    {
        right = screen_width;
    }
    int64_t bottom = y + request.height;
    if (bottom > screen_height)
    {
        bottom = screen_height;
    }
    if (right <= left || bottom <= top)
    {
        return 0;
    }
    clipped->screen_x = static_cast<uint32_t>(left);
    clipped->screen_y = static_cast<uint32_t>(top);
    clipped->width = static_cast<uint32_t>(right - left);
    clipped->height = static_cast<uint32_t>(bottom - top);
    clipped->source_offset =
        static_cast<uint64_t>((top - y) * static_cast<int64_t>(request.stride) + (left - x));
    return 0;
}

bool EventQueue::Push(const User::FramebufferEvent& event, uint32_t limit)
{
    if (m_size >= limit || m_size >= User::FramebufferEventCapacity)
    {
        ++m_dropped;
        return false;
    }
    m_events[(m_head + m_size) % User::FramebufferEventCapacity] = event;
    ++m_size;
    return true;
}

uint32_t EventQueue::Pop(User::FramebufferEvent* events, uint32_t capacity)
{
    uint32_t taken = 0;
    while (taken < capacity && m_size > 0)
    {
        events[taken++] = m_events[m_head];
        m_head = (m_head + 1) % User::FramebufferEventCapacity;
        --m_size;
    }
    return taken;
}

uint32_t EventQueue::Size() const
{
    return m_size;
}

uint32_t EventQueue::Dropped() const
{
    return m_dropped;
}

void EventQueue::Clear()
{
    m_head = 0;
    m_size = 0;
    m_dropped = 0;
}

namespace
{

User::FramebufferEvent StateEvent(uint8_t kind, uint32_t time_ms)
{
    return {0, kind, 0, time_ms};
}

} // namespace

uint64_t Owners::Acquire(uint32_t console, const void* owner, bool shown, uint32_t time_ms,
                         uint64_t flags)
{
    if (console >= Console::Count || owner == nullptr)
    {
        return User::SyscallErrorInvalidArgument;
    }
    Slot& slot = m_slots[console];
    if (slot.owner == owner)
    {
        return 0;
    }
    if (slot.owner != nullptr || ConsoleOf(owner) != Console::Count)
    {
        return User::SyscallErrorBusy;
    }
    slot = {};
    slot.owner = owner;
    slot.shown = shown;
    slot.raw_keys = (flags & User::FramebufferAcquireRawKeys) != 0;
    slot.queue.Push(
        StateEvent(shown ? User::FramebufferEventShown : User::FramebufferEventHidden, time_ms),
        User::FramebufferEventCapacity);
    return 0;
}

uint32_t Owners::Release(const void* owner)
{
    const uint32_t console = ConsoleOf(owner);
    if (console != Console::Count)
    {
        m_slots[console] = {};
    }
    return console;
}

bool Owners::Owns(uint32_t console, const void* owner) const
{
    return console < Console::Count && owner != nullptr && m_slots[console].owner == owner;
}

bool Owners::Acquired(uint32_t console) const
{
    return console < Console::Count && m_slots[console].owner != nullptr;
}

bool Owners::RawKeys(uint32_t console) const
{
    return console < Console::Count && m_slots[console].owner != nullptr &&
           m_slots[console].raw_keys;
}

uint32_t Owners::ConsoleOf(const void* owner) const
{
    for (uint32_t console = 0; console < Console::Count; ++console)
    {
        if (owner != nullptr && m_slots[console].owner == owner)
        {
            return console;
        }
    }
    return Console::Count;
}

const Owners::Slot* Owners::SlotOf(const void* owner) const
{
    const uint32_t console = ConsoleOf(owner);
    return console == Console::Count ? nullptr : &m_slots[console];
}

Owners::Slot* Owners::SlotOf(const void* owner)
{
    const uint32_t console = ConsoleOf(owner);
    return console == Console::Count ? nullptr : &m_slots[console];
}

bool Owners::DeliverKey(uint32_t console, const Input::KeyEvent& event, uint32_t time_ms)
{
    if (console >= Console::Count || m_slots[console].owner == nullptr || !m_slots[console].shown ||
        event.code >= Input::Keyboard::TrackedKeys)
    {
        return false;
    }
    Slot& slot = m_slots[console];
    uint64_t& word = slot.seen[event.code / 64];
    const uint64_t bit = uint64_t{1} << (event.code % 64);
    const bool seen = (word & bit) != 0;
    switch (event.transition)
    {
    case Input::KeyTransition::Press:
    case Input::KeyTransition::Repeat:
        if (slot.queue.Push({event.code,
                             seen ? User::FramebufferEventRepeat : User::FramebufferEventPress,
                             event.modifiers, time_ms},
                            KeyEventLimit))
        {
            word |= bit;
        }
        break;
    case Input::KeyTransition::Release:
        if (seen &&
            slot.queue.Push({event.code, User::FramebufferEventRelease, event.modifiers, time_ms},
                            KeyEventLimit))
        {
            word &= ~bit;
        }
        break;
    case Input::KeyTransition::None:
        break;
    }
    return true;
}

void Owners::Hide(uint32_t console, uint8_t modifiers, uint32_t time_ms)
{
    if (console >= Console::Count || m_slots[console].owner == nullptr || !m_slots[console].shown)
    {
        return;
    }
    Slot& slot = m_slots[console];
    for (uint32_t code = 0; code < Input::Keyboard::TrackedKeys; ++code)
    {
        if ((slot.seen[code / 64] & (uint64_t{1} << (code % 64))) != 0)
        {
            slot.queue.Push(
                {static_cast<uint16_t>(code), User::FramebufferEventRelease, modifiers, time_ms},
                KeyEventLimit);
        }
    }
    for (uint64_t& word : slot.seen)
    {
        word = 0;
    }
    slot.queue.Push(StateEvent(User::FramebufferEventHidden, time_ms),
                    User::FramebufferEventCapacity);
    slot.shown = false;
}

void Owners::Show(uint32_t console, uint32_t time_ms)
{
    if (console >= Console::Count || m_slots[console].owner == nullptr || m_slots[console].shown)
    {
        return;
    }
    Slot& slot = m_slots[console];
    slot.queue.Push(StateEvent(User::FramebufferEventShown, time_ms),
                    User::FramebufferEventCapacity);
    slot.shown = true;
}

uint64_t Owners::TakeEvents(const void* owner, User::FramebufferEvent* events, uint32_t capacity)
{
    Slot* slot = SlotOf(owner);
    return slot == nullptr ? User::SyscallErrorInvalidArgument : slot->queue.Pop(events, capacity);
}

uint32_t Owners::PendingEvents(const void* owner) const
{
    const Slot* slot = SlotOf(owner);
    return slot == nullptr ? 0 : slot->queue.Size();
}

uint32_t Owners::DroppedEvents(const void* owner) const
{
    const Slot* slot = SlotOf(owner);
    return slot == nullptr ? 0 : slot->queue.Dropped();
}

} // namespace FeatherOS::GraphicsMode
