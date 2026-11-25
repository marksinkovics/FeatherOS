#include "register_wait.h"

#include <algorithm>

namespace FeatherOS::Storage::Ahci
{
namespace
{

constexpr uint32_t Gone32 = 0xFFFFFFFF;

struct Wait
{
    ControllerHardware* hardware;
    uint32_t offset;
    uint32_t mask;
    uint32_t value;
};

bool Ready(void* context)
{
    const auto* wait = static_cast<const Wait*>(context);
    const uint32_t current = wait->hardware->Read32(wait->offset);
    return current != Gone32 && (current & wait->mask) == wait->value;
}

} // namespace

bool WaitForRegister(ControllerHardware& hardware, Environment& environment, const void* channel,
                     uint32_t offset, uint32_t mask, uint32_t value, uint32_t timeout_ms)
{
    Wait wait{&hardware, offset, mask, value};
    const uint64_t deadline = environment.NowMilliseconds() + timeout_ms;
    for (;;)
    {
        const uint64_t now = environment.NowMilliseconds();
        const uint64_t until = std::min<uint64_t>(now + WaitStepMs, deadline);
        if (environment.WaitUntil(channel, until, Ready, &wait))
        {
            return true;
        }
        if (environment.NowMilliseconds() >= deadline)
        {
            return false;
        }
    }
}

} // namespace FeatherOS::Storage::Ahci
