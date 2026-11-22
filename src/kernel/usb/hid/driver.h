#ifndef FEATHEROS_USB_HID_DRIVER_H
#define FEATHEROS_USB_HID_DRIVER_H

#include "kernel/usb/driver.h"
#include "kernel/usb/host_controller.h"

#include <stdint.h>

namespace FeatherOS::Input
{
class Keyboard;
} // namespace FeatherOS::Input

namespace FeatherOS::Usb::Hid
{

constexpr uint32_t MaxPorts = 32;
constexpr uint64_t NoDeadline = UINT64_MAX;

// Boot keyboards, boot mice, and any other HID interface (bound by its report descriptor).
extern const Driver KeyboardDriver;
extern const Driver MouseDriver;
extern const Driver GenericDriver;

// Where keys go; Input::SubmitKey unless a test replaces it.
using KeySink = void (*)(Input::Keyboard& keyboard, uint16_t code, bool pressed);
void SetKeySink(KeySink sink);

// Called by a controller's event task: submits the repeats that are due, and says when the next
// one is. Neither blocks, and neither is called with a USB lock held.
void ServiceRepeat(HostController& host, uint64_t now_milliseconds);
uint64_t NextRepeat(HostController& host);

// Called by the manager task: sends the global lock state to every keyboard whose LEDs differ.
void SendLeds();

} // namespace FeatherOS::Usb::Hid

#endif
