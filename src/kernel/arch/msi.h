#ifndef FEATHEROS_ARCH_MSI_H
#define FEATHEROS_ARCH_MSI_H

#include "kernel/pci/msi.h"

#include <stdint.h>

namespace FeatherOS::Arch::Msi
{

using Handler = void (*)(void* context);

// Hands out one message and calls handler(context) on the boot CPU when a device writes it.
// Everything is registered before it returns, so the message may be programmed at once.
bool Allocate(Handler handler, void* context, FeatherOS::Pci::MsiMessage* message);

// A level-triggered interrupt from RouteIntx, so the handler must quiet the device or it fires
// again at once. x86-64 has none and always fails; so does an irq that already has a handler.
bool RegisterIntx(uint32_t irq, Handler handler, void* context);

} // namespace FeatherOS::Arch::Msi

#endif
