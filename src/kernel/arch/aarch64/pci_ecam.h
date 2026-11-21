#ifndef FEATHEROS_AARCH64_PCI_ECAM_H
#define FEATHEROS_AARCH64_PCI_ECAM_H

#include "pci_device_tree.h"

namespace FeatherOS::Arch::AArch64::PciEcam
{

// The host bridge Arch::Pci::Initialize found, or nullptr before it succeeded.
const PciDeviceTree::HostBridge* Bridge();

} // namespace FeatherOS::Arch::AArch64::PciEcam

#endif
