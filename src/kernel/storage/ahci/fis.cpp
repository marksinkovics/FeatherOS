#include "fis.h"

namespace FeatherOS::Storage::Ahci
{
namespace
{

constexpr uint32_t WriteFlag = 1U << 6;
constexpr uint32_t PrdtlShift = 16;
constexpr uint32_t InterruptFlag = 1U << 31;
constexpr uint32_t ByteCountMask = 0x3FFFFF;

constexpr uint32_t SignatureAta = 0x00000101;
constexpr uint32_t SignatureAtapi = 0xEB140101;
constexpr uint32_t SignaturePortMultiplier = 0x96690101;
constexpr uint32_t SignatureSemb = 0xC33C0101;

} // namespace

H2dFis BuildH2d(uint8_t command, uint64_t lba, uint16_t count)
{
    H2dFis fis{};
    fis.type = FisTypeH2d;
    fis.flags = FisCommandBit;
    fis.command = command;
    fis.device = DeviceLba;
    for (uint32_t i = 0; i < 3; i++)
    {
        fis.lba[i] = static_cast<uint8_t>(lba >> (8 * i));
        fis.lba_high[i] = static_cast<uint8_t>(lba >> (24 + 8 * i));
    }
    fis.count[0] = static_cast<uint8_t>(count);
    fis.count[1] = static_cast<uint8_t>(count >> 8);
    return fis;
}

std::optional<H2dFis> BuildTransfer(bool write, uint64_t lba, uint32_t sectors)
{
    if (sectors == 0 || sectors > MaxSectorsPerCommand || lba >= LbaLimit ||
        lba + sectors > LbaLimit)
    {
        return std::nullopt;
    }
    // A zero count in a 48-bit command means 65536 sectors.
    return BuildH2d(write ? AtaCommand::WriteDmaExt : AtaCommand::ReadDmaExt, lba,
                    static_cast<uint16_t>(sectors));
}

H2dFis BuildNoData(uint8_t command)
{
    return BuildH2d(command, 0, 0);
}

std::optional<CommandHeader> BuildCommandHeader(bool write, uint16_t prd_count,
                                                uint64_t table_physical)
{
    if ((table_physical & (CommandTableAlignment - 1)) != 0)
    {
        return std::nullopt;
    }
    CommandHeader header{};
    header.flags = CommandFisLengthDwords | (write ? WriteFlag : 0U) |
                   (static_cast<uint32_t>(prd_count) << PrdtlShift);
    header.table_low = static_cast<uint32_t>(table_physical);
    header.table_high = static_cast<uint32_t>(table_physical >> 32);
    return header;
}

std::optional<Prd> BuildPrd(uint64_t address, uint32_t bytes, bool interrupt)
{
    if (bytes == 0 || bytes > MaxPrdBytes || (bytes & 1) != 0 || (address & 1) != 0)
    {
        return std::nullopt;
    }
    Prd prd{};
    prd.address_low = static_cast<uint32_t>(address);
    prd.address_high = static_cast<uint32_t>(address >> 32);
    prd.count = (bytes - 1) | (interrupt ? InterruptFlag : 0U);
    return prd;
}

uint32_t PrdByteCount(const Prd& prd)
{
    return (prd.count & ByteCountMask) + 1;
}

DeviceKind ClassifySignature(uint32_t signature)
{
    switch (signature)
    {
    case SignatureAta:
        return DeviceKind::Ata;
    case SignatureAtapi:
        return DeviceKind::Atapi;
    case SignaturePortMultiplier:
        return DeviceKind::PortMultiplier;
    case SignatureSemb:
        return DeviceKind::Semb;
    default:
        return DeviceKind::Unknown;
    }
}

const char* DeviceKindText(DeviceKind kind)
{
    switch (kind)
    {
    case DeviceKind::Ata:
        return "ATA disk";
    case DeviceKind::Atapi:
        return "ATAPI device";
    case DeviceKind::PortMultiplier:
        return "port multiplier";
    case DeviceKind::Semb:
        return "enclosure management bridge";
    case DeviceKind::Unknown:
        return "unknown device";
    }
    return "unknown device";
}

} // namespace FeatherOS::Storage::Ahci
