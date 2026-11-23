#include <catch2/catch_test_macros.hpp>

#include "kernel/boot_options.h"
#include "kernel/storage/write_policy.h"

using FeatherOS::BootOptions::StorageMode;
using FeatherOS::Storage::DecideWrites;
using FeatherOS::Storage::WritePolicy;
using FeatherOS::Storage::WritesAllowed;

namespace
{

struct Controller
{
    const char* name;
    uint16_t subsystem_vendor;
    uint16_t subsystem;
    bool qemu;
};

constexpr Controller Controllers[] = {
    {"QEMU", 0x1AF4, 0x1100, true},
    {"Samsung NVMe", 0x144D, 0xA801, false},
    {"Intel AHCI", 0x8086, 0x7270, false},
    {"no subsystem", 0x0000, 0x0000, false},
    {"QEMU vendor, other subsystem", 0x1AF4, 0x0001, false},
    {"QEMU subsystem, other vendor", 0x8086, 0x1100, false},
};

} // namespace

TEST_CASE("storage T4: the write policy for every mode and controller")
{
    for (const Controller& controller : Controllers)
    {
        CAPTURE(controller.name);
        const auto decide = [&](StorageMode mode)
        { return DecideWrites(mode, controller.subsystem_vendor, controller.subsystem); };

        REQUIRE(decide(StorageMode::Auto) ==
                (controller.qemu ? WritePolicy::Allowed : WritePolicy::ReadOnlyHardware));
        REQUIRE(decide(StorageMode::ReadOnly) == WritePolicy::ReadOnlyOption);
        REQUIRE(decide(StorageMode::ReadWrite) ==
                (controller.qemu ? WritePolicy::Allowed : WritePolicy::AllowedByOption));
    }
}

TEST_CASE("storage T4: only QEMU's controllers write in auto mode, storage=ro stops every one")
{
    REQUIRE(WritesAllowed(DecideWrites(StorageMode::Auto, 0x1AF4, 0x1100)));
    REQUIRE_FALSE(WritesAllowed(DecideWrites(StorageMode::Auto, 0x144D, 0xA801)));
    REQUIRE_FALSE(WritesAllowed(DecideWrites(StorageMode::ReadOnly, 0x1AF4, 0x1100)));
    REQUIRE_FALSE(WritesAllowed(DecideWrites(StorageMode::ReadOnly, 0x144D, 0xA801)));
    REQUIRE(WritesAllowed(DecideWrites(StorageMode::ReadWrite, 0x144D, 0xA801)));
}
