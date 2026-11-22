#ifndef FEATHEROS_X86_64_ACPI_H
#define FEATHEROS_X86_64_ACPI_H

#include <stdint.h>

namespace FeatherOS::Arch::X86_64::Acpi
{

constexpr uint32_t SdtHeaderSize = 36;
constexpr uint16_t BootArchitecture8042 = 1U << 1;

uint32_t Read32(const uint8_t* bytes);
uint64_t Read64(const uint8_t* bytes);
const uint8_t* MapTable(uint64_t physical_address, const char* signature);
const uint8_t* FindTable(uint64_t rsdp_physical, const char* signature);

void RecordFadt(const uint8_t* fadt);
bool BootArchitectureFlags(uint16_t* flags);

} // namespace FeatherOS::Arch::X86_64::Acpi

#endif
