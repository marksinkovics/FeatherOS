#include "acpi.h"

#include "kernel/memory/device_memory.h"

#include <stddef.h>

namespace FeatherOS::Arch::X86_64::Acpi
{
namespace
{

constexpr uint32_t BootArchitectureOffset = 109;
// Before revision 3 the same bytes hold other fields.
constexpr uint8_t FadtWithBootArchitecture = 3;
constexpr uint32_t BootArchitectureEnd = BootArchitectureOffset + 2;

bool boot_architecture_known = false;
uint16_t boot_architecture = 0;

bool SumsToZero(const uint8_t* bytes, uint32_t length)
{
    uint8_t sum = 0;
    for (uint32_t index = 0; index < length; ++index)
    {
        sum = static_cast<uint8_t>(sum + bytes[index]);
    }
    return sum == 0;
}

} // namespace

uint32_t Read32(const uint8_t* bytes)
{
    return static_cast<uint32_t>(bytes[0]) | (static_cast<uint32_t>(bytes[1]) << 8) |
           (static_cast<uint32_t>(bytes[2]) << 16) | (static_cast<uint32_t>(bytes[3]) << 24);
}

uint64_t Read64(const uint8_t* bytes)
{
    return static_cast<uint64_t>(Read32(bytes)) | (static_cast<uint64_t>(Read32(bytes + 4)) << 32);
}

// The firmware's tables are outside the HHDM, so each is mapped through the device window.
const uint8_t* MapTable(uint64_t physical_address, const char* signature)
{
    const auto* header =
        reinterpret_cast<const uint8_t*>(Memory::MapDevice(physical_address, SdtHeaderSize));
    if (header == nullptr)
    {
        return nullptr;
    }
    for (uint32_t index = 0; index < 4; ++index)
    {
        if (header[index] != static_cast<uint8_t>(signature[index]))
        {
            return nullptr;
        }
    }
    const uint32_t length = Read32(header + 4);
    const auto* table =
        reinterpret_cast<const uint8_t*>(Memory::MapDevice(physical_address, length));
    return length >= SdtHeaderSize && table != nullptr && SumsToZero(table, length) ? table
                                                                                    : nullptr;
}

const uint8_t* FindTable(uint64_t rsdp_physical, const char* signature)
{
    const auto* rsdp = reinterpret_cast<const uint8_t*>(Memory::MapDevice(rsdp_physical, 36));
    constexpr char RsdpSignature[] = "RSD PTR ";
    for (uint32_t index = 0; rsdp != nullptr && index < 8; ++index)
    {
        if (rsdp[index] != static_cast<uint8_t>(RsdpSignature[index]))
        {
            return nullptr;
        }
    }
    if (rsdp == nullptr || !SumsToZero(rsdp, 20))
    {
        return nullptr;
    }
    // Revision 2 added the XSDT, whose entries are 64-bit.
    const bool extended = rsdp[15] >= 2 && Read64(rsdp + 24) != 0 && SumsToZero(rsdp, 36);
    const uint8_t* root =
        extended ? MapTable(Read64(rsdp + 24), "XSDT") : MapTable(Read32(rsdp + 16), "RSDT");
    if (root == nullptr)
    {
        return nullptr;
    }
    const uint32_t entry_size = extended ? 8 : 4;
    const uint32_t entry_count = (Read32(root + 4) - SdtHeaderSize) / entry_size;
    for (uint32_t index = 0; index < entry_count; ++index)
    {
        const uint8_t* entry = root + SdtHeaderSize + static_cast<size_t>(index) * entry_size;
        const uint8_t* table = MapTable(extended ? Read64(entry) : Read32(entry), signature);
        if (table != nullptr)
        {
            return table;
        }
    }
    return nullptr;
}

void RecordFadt(const uint8_t* fadt)
{
    boot_architecture_known =
        fadt[8] >= FadtWithBootArchitecture && Read32(fadt + 4) >= BootArchitectureEnd;
    boot_architecture = boot_architecture_known
                            ? static_cast<uint16_t>(fadt[BootArchitectureOffset] |
                                                    (fadt[BootArchitectureOffset + 1] << 8))
                            : 0;
}

bool BootArchitectureFlags(uint16_t* flags)
{
    *flags = boot_architecture;
    return boot_architecture_known;
}

} // namespace FeatherOS::Arch::X86_64::Acpi
