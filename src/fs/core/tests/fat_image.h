// SPDX-License-Identifier: MIT OR Apache-2.0
#ifndef FEATHERDISK_TESTS_FAT_IMAGE_H
#define FEATHERDISK_TESTS_FAT_IMAGE_H

#include "tests/filesystem_helpers.h"
#include "tests/memory_device.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace FeatherDisk::Tests
{

// A small FAT16 volume without a partition table: one reserved sector, the FATs, the root
// directory, then one-sector clusters.
struct Fat16Layout
{
    std::uint32_t sectors = 5000;
    std::uint32_t fat_count = 1;
    std::uint32_t fat_sectors = 20;
    std::uint32_t root_entries = 16;
};

class Fat16Image
{
public:
    explicit Fat16Image(Fat16Layout image_layout = {});

    MemoryDevice& Device();
    std::uint32_t RootDirectorySector() const;
    std::uint8_t* RootEntry(std::uint32_t index);
    std::uint16_t FatEntry(std::uint32_t cluster, std::uint32_t copy = 0);
    // In every FAT copy.
    void SetFatEntry(std::uint32_t cluster, std::uint16_t value);
    // BLOCK.TXT in the root's first entry, in cluster 2 and, past 512 bytes, cluster 3.
    void AddRootFile(const std::string& contents);

private:
    Fat16Layout layout;
    MemoryDevice device;
};

// A volume read the way an independent checker would: every live entry, who owns each cluster,
// and the clusters allocated but owned by nobody. It follows the on-disk bytes only, not the
// driver.
struct FatEntryInfo
{
    // The names a scan shows, joined with '/': long name, or else short name.
    std::string path;
    std::string short_name;
    std::uint32_t directory;
    std::uint32_t slot;
    std::uint32_t first_cluster;
    std::uint32_t size;
    bool is_directory;
    std::vector<std::uint32_t> chain;
};

struct FatScan
{
    std::vector<FatEntryInfo> entries;
    // Cluster to the paths of the entries whose chain holds it.
    std::map<std::uint32_t, std::vector<std::string>> owners;
    std::vector<std::uint32_t> lost;
    // Clusters some entry's chain runs through although the FAT marks them free.
    std::vector<std::uint32_t> freed_but_owned;

    // Ignoring case; nullptr when there is none.
    const FatEntryInfo* Find(const std::string& path) const;
    // Entries whose first cluster is cluster.
    std::vector<const FatEntryInfo*> Referencing(std::uint32_t cluster) const;
};

FatScan ScanFat16(MemoryDevice& device);
// The bytes of a file's chain, size bytes of them.
std::string FatContents(MemoryDevice& device, const FatEntryInfo& entry);
std::uint16_t FatValue(MemoryDevice& device, std::uint32_t cluster);

} // namespace FeatherDisk::Tests

#endif
