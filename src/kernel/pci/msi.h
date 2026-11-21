#ifndef FEATHEROS_PCI_MSI_H
#define FEATHEROS_PCI_MSI_H

#include "kernel/pci/pci.h"

#include <stdint.h>

namespace FeatherOS::Pci
{

struct MsiMessage
{
    uint64_t address;
    uint32_t data;
};

// table is the mapped MSI-X table of the function. Both calls program the message first, then set
// the enable bit with INTx disabled, and leave bus mastering to EnableDevice.
bool EnableMsiX(ConfigSpace& config, const Function& function, uintptr_t table, uint16_t entry,
                const MsiMessage& message);
bool EnableMsi(ConfigSpace& config, const Function& function, const MsiMessage& message);

} // namespace FeatherOS::Pci

#endif
