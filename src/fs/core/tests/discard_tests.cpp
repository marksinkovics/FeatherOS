// SPDX-License-Identifier: MIT OR Apache-2.0
// FilesystemType::Discard frees a volume whose device is gone: no device call, nothing leaked.
#include <catch2/catch_test_macros.hpp>

#include "fs/ext2.h"
#include "fs/fat.h"
#include "tests/ext2_image.h"
#include "tests/fat_image.h"
#include "tests/filesystem_helpers.h"
#include "tests/memory_device.h"

#include <cstdint>
#include <memory>
#include <string>

namespace
{

using namespace FeatherDisk;
using namespace FeatherDisk::Tests;

struct DeviceCalls
{
    std::uint64_t reads;
    std::uint64_t writes;
    std::uint32_t flushes;

    bool operator==(const DeviceCalls&) const = default;
};

DeviceCalls CallsOf(const MemoryDevice& device)
{
    return {device.read_count, device.write_count, device.flush_count};
}

void WriteDirty(Filesystem& filesystem)
{
    WriteNewFile(filesystem, "a.txt", "alpha\n");
    WriteNewFile(filesystem, "big.bin", Pattern(12000));
}

void DiscardDirty(FilesystemType& type, MemoryDevice& device)
{
    Filesystem* filesystem = Value(type.Mount(device, {.read_only = false}));
    REQUIRE(filesystem->GetCapabilities().writable);
    WriteDirty(*filesystem);
    const DeviceCalls before = CallsOf(device);
    type.Discard(*filesystem);
    REQUIRE(CallsOf(device) == before);
}

} // namespace

TEST_CASE("Discard frees a dirty FAT16 volume without touching the device")
{
    Fat16Image image;
    DiscardDirty(Fat::Fat16Type(), image.Device());
}

TEST_CASE("Discard frees a read-only FAT16 volume without touching the device")
{
    Fat16Image image;
    Filesystem* filesystem = Value(Fat::Fat16Type().Mount(image.Device(), {.read_only = true}));
    const DeviceCalls before = CallsOf(image.Device());
    Fat::Fat16Type().Discard(*filesystem);
    REQUIRE(CallsOf(image.Device()) == before);
}

TEST_CASE("Discard frees dirty ext2, ext3 and ext4 volumes without touching the device")
{
#ifndef FEATHERDISK_EXT_IMAGES
    SKIP("e2fsprogs is not installed");
#else
    struct Variant
    {
        std::string_view image;
        FilesystemType& (*type)();
    };
    for (const Variant& variant : {Variant{"1k", Ext::Ext2Type}, Variant{"ext3", Ext::Ext3Type},
                                   Variant{"ext4", Ext::Ext4Type}})
    {
        INFO(variant.image);
        const std::unique_ptr<MemoryDevice> device = DeviceFrom(LoadExtImage(variant.image));
        DiscardDirty(variant.type(), *device);
    }
#endif
}

TEST_CASE("Discard frees a read-only ext volume with a journal without touching the device")
{
#ifndef FEATHERDISK_EXT_IMAGES
    SKIP("e2fsprogs is not installed");
#else
    const std::unique_ptr<MemoryDevice> device = DeviceFrom(LoadExtImage("ext4"));
    Filesystem* filesystem = Value(Ext::Ext4Type().Mount(*device, {.read_only = true}));
    const DeviceCalls before = CallsOf(*device);
    Ext::Ext4Type().Discard(*filesystem);
    REQUIRE(CallsOf(*device) == before);
#endif
}
