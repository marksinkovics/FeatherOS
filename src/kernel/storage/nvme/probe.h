#ifndef FEATHEROS_STORAGE_NVME_PROBE_H
#define FEATHEROS_STORAGE_NVME_PROBE_H

#include "kernel/pci/pci.h"

namespace FeatherOS::Storage::Nvme
{

// Maps BAR 0, decides the write policy and posts the controller's bring-up to the storage
// worker. False when the controller could not be set up at all.
bool Probe(Pci::ConfigSpace& config, const Pci::Function& function);

} // namespace FeatherOS::Storage::Nvme

#endif
