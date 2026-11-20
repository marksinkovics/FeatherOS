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

uint64_t Owners::Acquire(uint32_t console, const void* owner)
{
    if (console >= Console::Count || owner == nullptr)
    {
        return User::SyscallErrorInvalidArgument;
    }
    if (m_owners[console] == owner)
    {
        return 0;
    }
    if (m_owners[console] != nullptr || ConsoleOf(owner) != Console::Count)
    {
        return User::SyscallErrorBusy;
    }
    m_owners[console] = owner;
    return 0;
}

uint32_t Owners::Release(const void* owner)
{
    const uint32_t console = ConsoleOf(owner);
    if (console != Console::Count)
    {
        m_owners[console] = nullptr;
    }
    return console;
}

bool Owners::Owns(uint32_t console, const void* owner) const
{
    return console < Console::Count && owner != nullptr && m_owners[console] == owner;
}

bool Owners::Acquired(uint32_t console) const
{
    return console < Console::Count && m_owners[console] != nullptr;
}

uint32_t Owners::ConsoleOf(const void* owner) const
{
    for (uint32_t console = 0; console < Console::Count; ++console)
    {
        if (owner != nullptr && m_owners[console] == owner)
        {
            return console;
        }
    }
    return Console::Count;
}

} // namespace FeatherOS::GraphicsMode
