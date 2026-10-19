#include <catch2/catch_test_macros.hpp>

#include <limine.h>

#include "kernel/memory/memory_map.h"

TEST_CASE("memory map normalizes region types and summarizes totals")
{
    limine_memmap_entry entries[3]{};
    entries[0].base = 0x1000;
    entries[0].length = 0x3000;
    entries[0].type = LIMINE_MEMMAP_USABLE;
    entries[1].base = 0x5000;
    entries[1].length = 0x1000;
    entries[1].type = LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE;
    entries[2].base = 0x6000;
    entries[2].length = 0x2000;
    entries[2].type = 0xFFFFFFFF;
    limine_memmap_entry* entry_pointers[] = {&entries[0], &entries[1], &entries[2]};
    limine_memmap_response response{};
    response.entry_count = 3;
    response.entries = entry_pointers;

    REQUIRE(FeatherOS::Memory::InitializeMemoryMapFromResponse(&response));
    FeatherOS::Memory::Summary summary{};
    REQUIRE(FeatherOS::Memory::GetSummary(&summary));
    REQUIRE(summary.region_count == 3);
    REQUIRE(summary.total_bytes == 0x6000);
    REQUIRE(summary.usable_bytes == 0x3000);
    REQUIRE(summary.reserved_bytes == 0x3000);

    FeatherOS::Memory::Region region{};
    REQUIRE(FeatherOS::Memory::GetRegion(1, &region));
    REQUIRE(region.base == 0x5000);
    REQUIRE(region.type == FeatherOS::Memory::RegionType::BootloaderReclaimable);
    REQUIRE_FALSE(FeatherOS::Memory::GetRegion(3, &region));
    REQUIRE_FALSE(FeatherOS::Memory::GetRegion(0, nullptr));

    REQUIRE_FALSE(FeatherOS::Memory::InitializeMemoryMapFromResponse(nullptr));
}

TEST_CASE("memory map rejects invalid spans and total-size overflow")
{
    limine_memmap_entry invalid_entry{};
    invalid_entry.base = UINT64_MAX - 7;
    invalid_entry.length = 8;
    invalid_entry.type = LIMINE_MEMMAP_USABLE;
    limine_memmap_entry* invalid_entry_pointers[] = {&invalid_entry};
    limine_memmap_response invalid_response{};
    invalid_response.entry_count = 1;
    invalid_response.entries = invalid_entry_pointers;
    REQUIRE_FALSE(FeatherOS::Memory::InitializeMemoryMapFromResponse(&invalid_response));
    REQUIRE(FeatherOS::Memory::RegionCount() == 0);

    limine_memmap_entry entries[2]{};
    entries[0].length = UINT64_MAX - 7;
    entries[0].type = LIMINE_MEMMAP_USABLE;
    entries[1].length = 8;
    entries[1].type = LIMINE_MEMMAP_RESERVED;
    limine_memmap_entry* overflow_entry_pointers[] = {&entries[0], &entries[1]};
    limine_memmap_response overflow_response{};
    overflow_response.entry_count = 2;
    overflow_response.entries = overflow_entry_pointers;
    REQUIRE_FALSE(FeatherOS::Memory::InitializeMemoryMapFromResponse(&overflow_response));
    FeatherOS::Memory::Summary summary{};
    REQUIRE_FALSE(FeatherOS::Memory::GetSummary(&summary));
}