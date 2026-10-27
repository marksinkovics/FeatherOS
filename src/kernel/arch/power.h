#ifndef FEATHEROS_ARCH_POWER_H
#define FEATHEROS_ARCH_POWER_H

#include <stdint.h>

// Firmware power control: ACPI on x86-64, PSCI on AArch64.
namespace FeatherOS::Arch::Power
{

// x86-64 reads the ACPI tables from the RSDP's physical address; AArch64 finds the PSCI conduit
// in the device tree. Either argument may be unused on the other architecture.
void Initialize(uint64_t acpi_rsdp_physical, const void* device_tree);
// Each returns only if the firmware did not act.
void PowerOff();
void Reset();

} // namespace FeatherOS::Arch::Power

#endif
