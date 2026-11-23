#ifndef FEATHEROS_STORAGE_NVME_IDENTIFY_H
#define FEATHEROS_STORAGE_NVME_IDENTIFY_H

#include <span>
#include <stdint.h>

namespace FeatherOS::Storage::Nvme
{

constexpr uint32_t IdentifyBytes = 4096;
constexpr uint32_t MaxNamespaces = 8;
constexpr uint32_t BounceBytes = 128 * 1024;

struct ControllerData
{
    uint16_t vendor_id;
    uint16_t subsystem_vendor_id;
    char serial[21];
    char model[41];
    char firmware[9];
    uint32_t version;
    uint32_t rtd3_entry_latency_us;
    // 0 means the controller states no limit.
    uint64_t max_transfer_bytes;
    uint8_t sq_entry_size_min_log2;
    uint8_t sq_entry_size_max_log2;
    uint8_t cq_entry_size_min_log2;
    uint8_t cq_entry_size_max_log2;
    uint32_t namespace_count;
    bool volatile_write_cache;
};

ControllerData ParseController(std::span<const uint8_t, IdentifyBytes> bytes);
bool SupportsEntrySizes(const ControllerData& data);
uint32_t BounceBufferBytes(const ControllerData& data);

enum class Refusal : uint8_t
{
    None,
    Inactive,
    BadFormat,
    Metadata,
    SectorSize,
    ProtectionInfo,
};

struct NamespaceData
{
    Refusal refusal;
    uint64_t sector_count;
    uint64_t used_sectors;
    uint32_t sector_size;
    uint16_t metadata_bytes;
    uint8_t format_index;
    bool write_protected;
};

NamespaceData ParseNamespace(std::span<const uint8_t, IdentifyBytes> bytes);
const char* RefusalText(Refusal refusal);

uint32_t ParseActiveList(std::span<const uint8_t, IdentifyBytes> bytes, std::span<uint32_t> out);

} // namespace FeatherOS::Storage::Nvme

#endif
