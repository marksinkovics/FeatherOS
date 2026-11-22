#include "driver.h"

#include "kernel/usb/hid/driver.h"

namespace FeatherOS::Usb
{
namespace
{

const Driver* const drivers[] = {&Hid::KeyboardDriver, &Hid::MouseDriver, &Hid::GenericDriver};

} // namespace

std::span<const Driver* const> BuiltInDrivers()
{
    return drivers;
}

} // namespace FeatherOS::Usb
