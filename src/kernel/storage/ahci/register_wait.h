#ifndef FEATHEROS_STORAGE_AHCI_REGISTER_WAIT_H
#define FEATHEROS_STORAGE_AHCI_REGISTER_WAIT_H

#include "kernel/storage/controller_hardware.h"
#include "kernel/storage/environment.h"

#include <stdint.h>

namespace FeatherOS::Storage::Ahci
{

constexpr uint32_t WaitStepMs = 10;

// True when (register & mask) == value within timeout_ms. A register that reads all ones counts as
// not reached, so a vanished HBA times out instead of passing a "bit clear" wait.
bool WaitForRegister(ControllerHardware& hardware, Environment& environment, const void* channel,
                     uint32_t offset, uint32_t mask, uint32_t value, uint32_t timeout_ms);

} // namespace FeatherOS::Storage::Ahci

#endif
