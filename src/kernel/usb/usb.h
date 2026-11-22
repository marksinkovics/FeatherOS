#ifndef FEATHEROS_USB_USB_H
#define FEATHEROS_USB_USB_H

namespace FeatherOS::Usb
{

// Finds the xHCI controllers, starts the event and manager tasks and returns whether any
// controller came up; with none, or usb=off, nothing changes.
bool Initialize(const void* platform_data);

} // namespace FeatherOS::Usb

#endif
