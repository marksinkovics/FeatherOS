#ifndef FEATHEROS_ARCH_PCI_H
#define FEATHEROS_ARCH_PCI_H

#include "kernel/pci/pci.h"

#include <stdint.h>

namespace FeatherOS::Arch::Pci
{

// Returns the config space of the platform's PCI host, or nullptr when there is none.
FeatherOS::Pci::ConfigSpace* Initialize(const void* platform_data);

// Gives the interrupt ID the platform routes the function's INTx pin to. x86-64 has none.
bool RouteIntx(const FeatherOS::Pci::Function& function, uint32_t* irq);

} // namespace FeatherOS::Arch::Pci

#endif
