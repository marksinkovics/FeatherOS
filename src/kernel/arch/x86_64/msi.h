#ifndef FEATHEROS_X86_64_MSI_H
#define FEATHEROS_X86_64_MSI_H

#include <stdint.h>

namespace FeatherOS::Arch::X86_64::Msi
{

void HandleInterrupt(uint64_t vector);

} // namespace FeatherOS::Arch::X86_64::Msi

#endif
