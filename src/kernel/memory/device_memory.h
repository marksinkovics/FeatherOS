#ifndef FEATHEROS_MEMORY_DEVICE_MEMORY_H
#define FEATHEROS_MEMORY_DEVICE_MEMORY_H

#include <stdint.h>

namespace FeatherOS::Memory
{

uintptr_t MapDevice(uint64_t physical_address, uint64_t size);

} // namespace FeatherOS::Memory

#endif
