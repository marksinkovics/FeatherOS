#ifndef FEATHEROS_STORAGE_AHCI_FIS_H
#define FEATHEROS_STORAGE_AHCI_FIS_H

#include <optional>
#include <stddef.h>
#include <stdint.h>

namespace FeatherOS::Storage::Ahci
{

namespace AtaCommand
{
constexpr uint8_t ReadDmaExt = 0x25;
constexpr uint8_t WriteDmaExt = 0x35;
constexpr uint8_t FlushCache = 0xE7;
constexpr uint8_t FlushCacheExt = 0xEA;
constexpr uint8_t StandbyImmediate = 0xE0;
constexpr uint8_t IdentifyDevice = 0xEC;
} // namespace AtaCommand

constexpr uint8_t FisTypeH2d = 0x27;
constexpr uint8_t FisCommandBit = 0x80;
constexpr uint8_t DeviceLba = 0x40;
constexpr uint32_t H2dFisBytes = 20;
constexpr uint32_t MaxSectorsPerCommand = 65536;
constexpr uint64_t LbaLimit = uint64_t{1} << 48;

// Register - Host to Device FIS (Serial ATA 3.x, "Register - Host to Device FIS"): byte 0 type,
// 1 PM port (3:0) and C (bit 7), 2 command, 3 features 7:0, 4-6 LBA 23:0, 7 device, 8-10 LBA 47:24,
// 11 features 15:8, 12-13 count, 14 ICC, 15 control, 16-19 reserved.
struct H2dFis
{
    uint8_t type;
    uint8_t flags;
    uint8_t command;
    uint8_t features_low;
    uint8_t lba[3];
    uint8_t device;
    uint8_t lba_high[3];
    uint8_t features_high;
    uint8_t count[2];
    uint8_t icc;
    uint8_t control;
    uint8_t reserved[4];
};

static_assert(sizeof(H2dFis) == H2dFisBytes);
static_assert(offsetof(H2dFis, command) == 2);
static_assert(offsetof(H2dFis, device) == 7);
static_assert(offsetof(H2dFis, lba_high) == 8);
static_assert(offsetof(H2dFis, count) == 12);

constexpr uint32_t CommandHeaderBytes = 32;
constexpr uint32_t CommandFisLengthDwords = 5;
constexpr uint32_t CommandTableAlignment = 128;

// Command header (AHCI 1.3.1 section 4.2.2): DW0 CFL 4:0, W bit 6, PRDTL 31:16; DW1 PRDBC; DW2-3
// CTBA, 128-byte aligned.
struct CommandHeader
{
    uint32_t flags;
    uint32_t prdbc;
    uint32_t table_low;
    uint32_t table_high;
    uint32_t reserved[4];
};

static_assert(sizeof(CommandHeader) == CommandHeaderBytes);
static_assert(offsetof(CommandHeader, prdbc) == 4);
static_assert(offsetof(CommandHeader, table_low) == 8);

constexpr uint32_t PrdBytes = 16;
constexpr uint32_t MaxPrdBytes = 4U * 1024 * 1024;

// PRD entry (AHCI 1.3.1 section 4.2.3): DW0-1 DBA (even), DW3 DBC 21:0 = bytes - 1 with bit 0 set,
// I bit 31.
struct Prd
{
    uint32_t address_low;
    uint32_t address_high;
    uint32_t reserved;
    uint32_t count;
};

static_assert(sizeof(Prd) == PrdBytes);
static_assert(offsetof(Prd, count) == 12);

H2dFis BuildH2d(uint8_t command, uint64_t lba, uint16_t count);
std::optional<H2dFis> BuildTransfer(bool write, uint64_t lba, uint32_t sectors);
H2dFis BuildNoData(uint8_t command);

std::optional<CommandHeader> BuildCommandHeader(bool write, uint16_t prd_count,
                                                uint64_t table_physical);
std::optional<Prd> BuildPrd(uint64_t address, uint32_t bytes, bool interrupt);
uint32_t PrdByteCount(const Prd& prd);

enum class DeviceKind : uint8_t
{
    Ata,
    Atapi,
    PortMultiplier,
    Semb,
    Unknown,
};

DeviceKind ClassifySignature(uint32_t signature);
const char* DeviceKindText(DeviceKind kind);

} // namespace FeatherOS::Storage::Ahci

#endif
