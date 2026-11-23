#include "driver.h"

#include "kernel/usb/hid/driver.h"
#include "kernel/usb/hub.h"
#include "kernel/usb/storage/driver.h"

namespace FeatherOS::Usb
{
namespace
{

const Driver* const drivers[] = {&HubDriver, &Hid::KeyboardDriver, &Hid::MouseDriver,
                                 &Hid::GenericDriver, &Storage::MassStorageDriver};

} // namespace

std::span<const Driver* const> BuiltInDrivers()
{
    return drivers;
}

} // namespace FeatherOS::Usb
