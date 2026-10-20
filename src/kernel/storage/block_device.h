#ifndef FEATHEROS_STORAGE_BLOCK_DEVICE_H
#define FEATHEROS_STORAGE_BLOCK_DEVICE_H

#include <stdint.h>

namespace FeatherOS::BlockDevice
{

constexpr uint32_t SectorSize = 512;

bool Initialize(const void* platform_data, uint64_t hhdm_offset);
uint64_t SectorCount();
bool ReadSector(uint64_t sector, void* buffer);
// Fails on a read-only device.
bool WriteSector(uint64_t sector, const void* buffer);

} // namespace FeatherOS::BlockDevice

#endif