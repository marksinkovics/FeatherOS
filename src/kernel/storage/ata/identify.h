#ifndef FEATHEROS_STORAGE_ATA_IDENTIFY_H
#define FEATHEROS_STORAGE_ATA_IDENTIFY_H

#include <span>
#include <stdint.h>

namespace FeatherOS::Storage::Ata
{

constexpr uint32_t IdentifyWords = 256;
constexpr uint32_t IdentifyBytes = IdentifyWords * 2;

enum class Refusal : uint8_t
{
    None,
    Checksum,
    NoLba48,
    SectorSize,
    Empty,
};

struct AtaIdentity
{
    Refusal refusal;
    uint64_t sector_count;
    uint32_t sector_size;
    bool lba48;
    bool write_cache_supported;
    bool write_cache_enabled;
    bool flush_cache_supported;
    bool flush_cache_ext_supported;
    bool checksum_present;
    char model[41];
    char serial[21];
    char firmware[9];
};

AtaIdentity ParseIdentify(std::span<const uint16_t, IdentifyWords> words);
const char* RefusalText(Refusal refusal);

} // namespace FeatherOS::Storage::Ata

#endif
