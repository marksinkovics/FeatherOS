#include <errno.h>
#include <featheros.h>
#include <featheros/framebuffer.h>
#include <stdarg.h>
#include <stddef.h>
#include <sys/ioctl.h>
#include <termios.h>

#include "libc/syscall_result.h"
#include "user/syscall.h"

using FeatherOS::LibC::Check;

static_assert(sizeof(fb_info) == sizeof(FeatherOS::User::FramebufferInfo) &&
              offsetof(fb_info, console) == offsetof(FeatherOS::User::FramebufferInfo, console) &&
              offsetof(fb_info, dropped_events) ==
                  offsetof(FeatherOS::User::FramebufferInfo, dropped_events));
static_assert(
    sizeof(fb_present) == sizeof(FeatherOS::User::FramebufferPresentRequest) &&
    offsetof(fb_present, stride) == offsetof(FeatherOS::User::FramebufferPresentRequest, stride) &&
    offsetof(fb_present, x) == offsetof(FeatherOS::User::FramebufferPresentRequest, x) &&
    offsetof(fb_present, width) == offsetof(FeatherOS::User::FramebufferPresentRequest, width) &&
    offsetof(fb_present, flags) == offsetof(FeatherOS::User::FramebufferPresentRequest, flags));
static_assert(sizeof(fb_event) == sizeof(FeatherOS::User::FramebufferEvent) &&
              offsetof(fb_event, code) == offsetof(FeatherOS::User::FramebufferEvent, code) &&
              offsetof(fb_event, kind) == offsetof(FeatherOS::User::FramebufferEvent, kind) &&
              offsetof(fb_event, modifiers) ==
                  offsetof(FeatherOS::User::FramebufferEvent, modifiers) &&
              offsetof(fb_event, time_ms) == offsetof(FeatherOS::User::FramebufferEvent, time_ms));
static_assert(FB_EVENT_RELEASE == FeatherOS::User::FramebufferEventRelease &&
              FB_EVENT_PRESS == FeatherOS::User::FramebufferEventPress &&
              FB_EVENT_REPEAT == FeatherOS::User::FramebufferEventRepeat &&
              FB_EVENT_SHOWN == FeatherOS::User::FramebufferEventShown &&
              FB_EVENT_HIDDEN == FeatherOS::User::FramebufferEventHidden &&
              FB_MOD_SHIFT == FeatherOS::User::FramebufferModifierShift &&
              FB_MOD_CONTROL == FeatherOS::User::FramebufferModifierControl &&
              FB_MOD_ALT == FeatherOS::User::FramebufferModifierAlt &&
              FB_MOD_CAPS_LOCK == FeatherOS::User::FramebufferModifierCapsLock &&
              FB_EVENT_CAPACITY == FeatherOS::User::FramebufferEventCapacity);
static_assert(FB_GET_INFO == FeatherOS::User::FramebufferGetInfo &&
              FB_ACQUIRE == FeatherOS::User::FramebufferAcquire &&
              FB_PRESENT == FeatherOS::User::FramebufferPresent &&
              FB_RELEASE == FeatherOS::User::FramebufferRelease);

namespace
{

constexpr unsigned long DeviceRequestClass = 0x46;

bool IsFramebufferRequest(unsigned long request)
{
    return (request >> 8) == DeviceRequestClass;
}

bool TakesArgument(unsigned long request)
{
    return request == FB_GET_INFO || request == FB_PRESENT;
}

} // namespace

int featheros_ioctl(int descriptor, unsigned long request, void* argument)
{
    if (descriptor < 0)
    {
        errno = EBADF;
        return -1;
    }
    if (IsFramebufferRequest(request))
    {
        return Check<int>(FeatherOS::User::DeviceControl(
            static_cast<uint64_t>(descriptor), request,
            TakesArgument(request) ? reinterpret_cast<uint64_t>(argument) : 0));
    }
    if (request != TIOCGWINSZ && request != TIOCSWINSZ)
    {
        // A terminal fails with EINVAL, anything else with ENOTTY.
        winsize unused;
        if (tcgetwinsize(descriptor, &unused) == 0)
        {
            errno = EINVAL;
        }
        return -1;
    }
    auto* size = static_cast<struct winsize*>(argument);
    return request == TIOCGWINSZ ? tcgetwinsize(descriptor, size) : tcsetwinsize(descriptor, size);
}

int ioctl(int descriptor, unsigned long request, ...)
{
    void* argument = nullptr;
    if (request == TIOCGWINSZ || request == TIOCSWINSZ || TakesArgument(request))
    {
        va_list arguments;
        va_start(arguments, request);
        argument = va_arg(arguments, void*);
        va_end(arguments);
    }
    return featheros_ioctl(descriptor, request, argument);
}
