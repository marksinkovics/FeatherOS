#include "kernel/arch/power.h"

#include "kernel/memory/device_memory.h"

namespace FeatherOS::Arch::Power
{
namespace
{

constexpr uint32_t SdtHeaderSize = 36;
constexpr uint16_t SciEnabled = 1U << 0;
constexpr uint16_t SleepTypeMask = 0x7U << 10;
constexpr uint16_t SleepEnable = 1U << 13;
constexpr uint32_t ResetRegisterSupported = 1U << 10;
constexpr uint8_t GasSystemIo = 1;
constexpr uint16_t KeyboardControllerPort = 0x64;
constexpr uint8_t KeyboardControllerBusy = 1U << 1;
constexpr uint8_t KeyboardControllerReset = 0xFE;
constexpr uint8_t AmlNameOp = 0x08;
constexpr uint8_t AmlPackageOp = 0x12;
constexpr uint8_t AmlBytePrefix = 0x0A;

uint32_t pm1a_control = 0;
uint32_t pm1b_control = 0;
uint32_t smi_command = 0;
uint8_t acpi_enable_value = 0;
uint16_t sleep_type_a = 0;
uint16_t sleep_type_b = 0;
bool can_power_off = false;
bool reset_register = false;
uint64_t reset_port = 0;
uint8_t reset_value = 0;

void WritePort8(uint16_t port, uint8_t value)
{
    asm volatile("outb %0, %1" : : "a"(value), "Nd"(port));
}

void WritePort16(uint16_t port, uint16_t value)
{
    asm volatile("outw %0, %1" : : "a"(value), "Nd"(port));
}

uint8_t ReadPort8(uint16_t port)
{
    uint8_t value;
    asm volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

uint16_t ReadPort16(uint16_t port)
{
    uint16_t value;
    asm volatile("inw %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

uint32_t Read32(const uint8_t* bytes)
{
    return static_cast<uint32_t>(bytes[0]) | (static_cast<uint32_t>(bytes[1]) << 8) |
           (static_cast<uint32_t>(bytes[2]) << 16) | (static_cast<uint32_t>(bytes[3]) << 24);
}

uint64_t Read64(const uint8_t* bytes)
{
    return static_cast<uint64_t>(Read32(bytes)) | (static_cast<uint64_t>(Read32(bytes + 4)) << 32);
}

bool SumsToZero(const uint8_t* bytes, uint32_t length)
{
    uint8_t sum = 0;
    for (uint32_t index = 0; index < length; ++index)
    {
        sum = static_cast<uint8_t>(sum + bytes[index]);
    }
    return sum == 0;
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
        const uint8_t* entry = root + SdtHeaderSize + index * entry_size;
        const uint8_t* table = MapTable(extended ? Read64(entry) : Read32(entry), signature);
        if (table != nullptr)
        {
            return table;
        }
    }
    return nullptr;
}

bool ReadPackageInteger(const uint8_t* aml, uint32_t length, uint32_t* position, uint16_t* value)
{
    if (*position >= length)
    {
        return false;
    }
    if (aml[*position] == AmlBytePrefix)
    {
        if (*position + 1 >= length)
        {
            return false;
        }
        *value = aml[*position + 1];
        *position += 2;
        return true;
    }
    // ZeroOp and OneOp encode 0 and 1 directly.
    *value = aml[*position];
    ++*position;
    return *value <= 1;
}

// Finds Name(_S5_, Package() {SLP_TYPa, SLP_TYPb, ...}) by scanning the DSDT's AML bytecode,
// which avoids an AML interpreter and works for the simple encodings firmware uses.
bool FindSoftOffSleepTypes(const uint8_t* dsdt, uint32_t length)
{
    for (uint32_t index = SdtHeaderSize + 1; index + 5 < length; ++index)
    {
        if (dsdt[index] != '_' || dsdt[index + 1] != 'S' || dsdt[index + 2] != '5' ||
            dsdt[index + 3] != '_' ||
            (dsdt[index - 1] != AmlNameOp &&
             (dsdt[index - 1] != '\\' || index < 2 || dsdt[index - 2] != AmlNameOp)) ||
            dsdt[index + 4] != AmlPackageOp)
        {
            continue;
        }
        // PkgLength's lead byte gives the count of extra length bytes in its top two bits; the
        // element count follows.
        uint32_t position = index + 5;
        position += 1 + (dsdt[position] >> 6) + 1;
        return ReadPackageInteger(dsdt, length, &position, &sleep_type_a) &&
               ReadPackageInteger(dsdt, length, &position, &sleep_type_b);
    }
    return false;
}

void WriteSleepControl(uint32_t port, uint16_t sleep_type)
{
    const auto control_port = static_cast<uint16_t>(port);
    const uint16_t control = ReadPort16(control_port);
    WritePort16(control_port, static_cast<uint16_t>((control & ~SleepTypeMask) |
                                                    (sleep_type << 10) | SleepEnable));
}

} // namespace

void Initialize(uint64_t acpi_rsdp_physical, const void*)
{
    const uint8_t* fadt = acpi_rsdp_physical == 0 ? nullptr : FindTable(acpi_rsdp_physical, "FACP");
    if (fadt == nullptr)
    {
        return;
    }
    const uint32_t fadt_length = Read32(fadt + 4);
    smi_command = Read32(fadt + 48);
    acpi_enable_value = fadt[52];
    pm1a_control = Read32(fadt + 64);
    pm1b_control = Read32(fadt + 68);
    // ACPI 2.0 FADTs add the reset register and a 64-bit DSDT address.
    if (fadt[8] >= 2 && fadt_length >= 129 && (Read32(fadt + 112) & ResetRegisterSupported) != 0 &&
        fadt[116] == GasSystemIo)
    {
        reset_register = true;
        reset_port = Read64(fadt + 120);
        reset_value = fadt[128];
    }
    const uint64_t dsdt_physical =
        fadt_length >= 148 && Read64(fadt + 140) != 0 ? Read64(fadt + 140) : Read32(fadt + 40);
    const uint8_t* dsdt = MapTable(dsdt_physical, "DSDT");
    can_power_off =
        pm1a_control != 0 && dsdt != nullptr && FindSoftOffSleepTypes(dsdt, Read32(dsdt + 4));
}

void PowerOff()
{
    if (!can_power_off)
    {
        return;
    }
    const auto control_port = static_cast<uint16_t>(pm1a_control);
    if ((ReadPort16(control_port) & SciEnabled) == 0 && smi_command != 0 && acpi_enable_value != 0)
    {
        WritePort8(static_cast<uint16_t>(smi_command), acpi_enable_value);
        for (uint32_t attempt = 0;
             attempt < 1000000 && (ReadPort16(control_port) & SciEnabled) == 0; ++attempt)
        {
        }
    }
    WriteSleepControl(pm1a_control, sleep_type_a);
    if (pm1b_control != 0)
    {
        WriteSleepControl(pm1b_control, sleep_type_b);
    }
    for (uint32_t attempt = 0; attempt < 10000000; ++attempt)
    {
        asm volatile("pause");
    }
}

void Reset()
{
    if (reset_register && reset_port <= UINT16_MAX)
    {
        WritePort8(static_cast<uint16_t>(reset_port), reset_value);
    }
    for (uint32_t attempt = 0;
         attempt < 100000 && (ReadPort8(KeyboardControllerPort) & KeyboardControllerBusy) != 0;
         ++attempt)
    {
    }
    WritePort8(KeyboardControllerPort, KeyboardControllerReset);
    for (uint32_t attempt = 0; attempt < 10000000; ++attempt)
    {
        asm volatile("pause");
    }
    // An exception with an empty IDT escalates to a triple fault, which resets the CPU.
    struct __attribute__((packed))
    {
        uint16_t limit;
        uint64_t base;
    } empty_idt = {0, 0};
    asm volatile("lidt %0\n\tint3" : : "m"(empty_idt) : "memory");
}

} // namespace FeatherOS::Arch::Power
