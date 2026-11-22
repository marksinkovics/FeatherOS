// SPDX-License-Identifier: MIT OR Apache-2.0
#include <catch2/catch_test_macros.hpp>

#include "common/bitmap.h"
#include "common/crc.h"
#include "common/host_buffer.h"
#include "common/little_endian.h"
#include "fs/filesystem.h"
#include "fs/host.h"

#include <array>
#include <cstdint>
#include <span>
#include <string_view>

using FeatherDisk::LoadLittleEndian;
using FeatherDisk::StoreLittleEndian;

TEST_CASE("little-endian fields load and store byte by byte at any alignment")
{
    std::array<std::uint8_t, 11> bytes{};
    StoreLittleEndian<std::uint16_t>(bytes.data() + 1, 0xBEEF);
    StoreLittleEndian<std::uint64_t>(bytes.data() + 3, 0x0123456789ABCDEFULL);
    REQUIRE(bytes == std::array<std::uint8_t, 11>{0x00, 0xEF, 0xBE, 0xEF, 0xCD, 0xAB, 0x89, 0x67,
                                                  0x45, 0x23, 0x01});
    REQUIRE(LoadLittleEndian<std::uint16_t>(bytes.data() + 1) == 0xBEEF);
    REQUIRE(LoadLittleEndian<std::uint32_t>(bytes.data() + 3) == 0x89ABCDEFU);
    REQUIRE(LoadLittleEndian<std::uint64_t>(bytes.data() + 3) == 0x0123456789ABCDEFULL);
    REQUIRE(LoadLittleEndian<std::uint8_t>(bytes.data() + 10) == 0x01);
}

TEST_CASE("CRC-32C and CRC-16 match their published check values and continue over pieces")
{
    using FeatherDisk::Crc16;
    using FeatherDisk::Crc32c;
    constexpr std::string_view Check = "123456789";
    const std::span<const std::uint8_t> check(reinterpret_cast<const std::uint8_t*>(Check.data()),
                                              Check.size());
    REQUIRE(~Crc32c(~0U, check) == 0xE3069283U);
    // RFC 3720's iSCSI vectors: 32 bytes of zeros, and of ones.
    std::array<std::uint8_t, 32> block{};
    REQUIRE(~Crc32c(~0U, block) == 0x8A9136AAU);
    block.fill(0xFF);
    REQUIRE(~Crc32c(~0U, block) == 0x62A8AB43U);
    REQUIRE(Crc32c(0x12345678U, {}) == 0x12345678U);
    REQUIRE(Crc32c(Crc32c(~0U, check.first(4)), check.subspan(4)) == Crc32c(~0U, check));

    // CRC-16/MODBUS, which is ext4's gdt_csum form seeded with ~0.
    REQUIRE(Crc16(0xFFFF, check) == 0x4B37);
    // CRC-16/ARC: the same polynomial from zero.
    REQUIRE(Crc16(0, check) == 0xBB3D);
    REQUIRE(Crc16(Crc16(0xFFFF, check.first(5)), check.subspan(5)) == Crc16(0xFFFF, check));
}

constexpr std::array<std::uint8_t, 4> CompileTimeBytes{1, 2, 3, 4};
static_assert(LoadLittleEndian<std::uint32_t>(CompileTimeBytes.data()) == 0x04030201U);

TEST_CASE("host buffers come from the host and may be empty")
{
    FeatherDisk::HostBuffer buffer(4096);
    REQUIRE_FALSE(buffer.Empty());
    REQUIRE(buffer.Bytes().size() == 4096);
    buffer.Bytes()[4095] = 7;

    FeatherDisk::HostBuffer nothing(0);
    REQUIRE(nothing.Empty());
    REQUIRE(nothing.Bytes().empty());
}

TEST_CASE("the standard host's hooks work")
{
    FeatherDisk::Host::Lock* lock = FeatherDisk::Host::CreateLock();
    REQUIRE(lock != nullptr);
    FeatherDisk::Host::Acquire(*lock);
    FeatherDisk::Host::Release(*lock);
    FeatherDisk::Host::DestroyLock(lock);
    // 2020-01-01.
    REQUIRE(FeatherDisk::Host::WallClockNanoseconds().value_or(0) > 1577836800ULL * 1000000000ULL);
    FeatherDisk::Host::Log(FeatherDisk::Host::LogLevel::Debug, "a test message");
}

namespace
{

class EmptyFilesystem final : public FeatherDisk::Filesystem
{
public:
    FeatherDisk::Capabilities GetCapabilities() const override
    {
        return {};
    }
    FeatherDisk::NodeId Root() const override
    {
        return 1;
    }
    FeatherDisk::Result<FeatherDisk::VolumeStatistics> Statistics() override
    {
        return FeatherDisk::VolumeStatistics{};
    }
    FeatherDisk::Result<FeatherDisk::NodeId> Lookup(FeatherDisk::NodeId /*directory*/,
                                                    std::string_view /*name*/) override
    {
        return std::unexpected(FeatherDisk::Error::NotFound);
    }
    FeatherDisk::Result<bool> ReadDirectory(FeatherDisk::NodeId /*directory*/,
                                            std::uint64_t& /*cookie*/,
                                            FeatherDisk::DirectoryEntry& /*entry*/) override
    {
        return false;
    }
    FeatherDisk::Result<FeatherDisk::Attributes> GetAttributes(FeatherDisk::NodeId node) override
    {
        FeatherDisk::Attributes attributes{};
        attributes.node = node;
        attributes.type = FeatherDisk::NodeType::Directory;
        return attributes;
    }
    FeatherDisk::Result<std::size_t> Read(FeatherDisk::NodeId /*file*/, std::uint64_t /*offset*/,
                                          std::span<std::uint8_t> /*buffer*/) override
    {
        return std::unexpected(FeatherDisk::Error::IsDirectory);
    }
};

} // namespace

TEST_CASE("a driver's left-out operations are unsupported, and syncing succeeds")
{
    EmptyFilesystem filesystem;
    const FeatherDisk::NodeId root = filesystem.Root();
    const auto unsupported = FeatherDisk::Error::Unsupported;
    std::array<std::uint8_t, 4> data{};
    std::array<char, 8> link{};
    FeatherDisk::AttributeChanges changes{};
    changes.mode = 0644;
    REQUIRE(filesystem.SetAttributes(root, changes).error() == unsupported);
    REQUIRE(filesystem.Write(root, 0, data).error() == unsupported);
    REQUIRE(filesystem.Truncate(root, 0).error() == unsupported);
    REQUIRE(filesystem.Create(root, "a", 0644).error() == unsupported);
    REQUIRE(filesystem.MakeDirectory(root, "d", 0755).error() == unsupported);
    REQUIRE(filesystem.Remove(root, "a").error() == unsupported);
    REQUIRE(filesystem.RemoveDirectory(root, "d").error() == unsupported);
    REQUIRE(filesystem.Rename(root, "a", root, "b").error() == unsupported);
    REQUIRE(filesystem.Link(root, root, "b").error() == unsupported);
    REQUIRE(filesystem.SymbolicLink(root, "l", "a").error() == unsupported);
    REQUIRE(filesystem.ReadLink(root, link).error() == unsupported);
    REQUIRE(filesystem.Sync().has_value());
}

TEST_CASE("bitmaps set, clear and find clear bits within a range")
{
    std::array<std::uint8_t, 4> bitmap{};
    FeatherDisk::SetBit(bitmap, 0);
    FeatherDisk::SetBit(bitmap, 9);
    REQUIRE(bitmap == std::array<std::uint8_t, 4>{0x01, 0x02, 0x00, 0x00});
    REQUIRE(FeatherDisk::TestBit(bitmap, 9));
    REQUIRE_FALSE(FeatherDisk::TestBit(bitmap, 8));
    FeatherDisk::ClearBit(bitmap, 9);
    REQUIRE_FALSE(FeatherDisk::TestBit(bitmap, 9));

    bitmap = {0xFF, 0xFF, 0x7F, 0x00};
    REQUIRE(FeatherDisk::FindClearBit(bitmap, 0, 32) == 23U);
    REQUIRE(FeatherDisk::FindClearBit(bitmap, 24, 32) == 24U);
    // The end is exclusive, and a range past the bitmap stops at its end.
    REQUIRE_FALSE(FeatherDisk::FindClearBit(bitmap, 0, 23).has_value());
    bitmap = {0xFF, 0xFF, 0xFF, 0xFF};
    REQUIRE_FALSE(FeatherDisk::FindClearBit(bitmap, 0, 1000).has_value());
    bitmap = {0xFF, 0xEF, 0xFF, 0xFF};
    REQUIRE(FeatherDisk::FindClearBit(bitmap, 3, 32) == 12U);
    REQUIRE_FALSE(FeatherDisk::FindClearBit(bitmap, 13, 32).has_value());
}
