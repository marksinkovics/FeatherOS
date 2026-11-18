// SPDX-License-Identifier: MIT OR Apache-2.0
// The jbd2 encoders against step 1's decoders, and logs they built (wrapped, with an escaped copy,
// a revoke and a torn tail) replayed by this core and by debugfs, which must agree.
#include <catch2/catch_test_macros.hpp>

#include "common/crc.h"
#include "ext/journal.h"
#include "fs/ext2.h"
#include "tests/ext2_image.h"
#include "tests/filesystem_helpers.h"
#include "tests/memory_device.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{

using FeatherDisk::Crc32c;
using FeatherDisk::Ext::CheckJournalSuperblock;
using FeatherDisk::Ext::DecodeJournalHeader;
using FeatherDisk::Ext::DecodeJournalSuperblock;
using FeatherDisk::Ext::EncodeCommitBlock;
using FeatherDisk::Ext::EncodeJournalFeatures;
using FeatherDisk::Ext::EncodeJournalPosition;
using FeatherDisk::Ext::EscapeJournalBlock;
using FeatherDisk::Ext::JournalCommitBlock;
using FeatherDisk::Ext::JournalCommitMatches;
using FeatherDisk::Ext::JournalCompatibleChecksum;
using FeatherDisk::Ext::JournalDescriptorBlock;
using FeatherDisk::Ext::JournalDescriptorWriter;
using FeatherDisk::Ext::JournalHasChecksums;
using FeatherDisk::Ext::JournalIncompatible64Bit;
using FeatherDisk::Ext::JournalIncompatibleChecksumV2;
using FeatherDisk::Ext::JournalIncompatibleChecksumV3;
using FeatherDisk::Ext::JournalIncompatibleRevoke;
using FeatherDisk::Ext::JournalMagic;
using FeatherDisk::Ext::JournalRevokeBlock;
using FeatherDisk::Ext::JournalRevokesPerBlock;
using FeatherDisk::Ext::JournalRevokeWriter;
using FeatherDisk::Ext::JournalSuperblock;
using FeatherDisk::Ext::JournalSuperblockVersion2;
using FeatherDisk::Ext::JournalTag;
using FeatherDisk::Ext::JournalTagChecksum;
using FeatherDisk::Ext::JournalTagEscaped;
using FeatherDisk::Ext::JournalTagLast;
using FeatherDisk::Ext::JournalTagMatches;
using FeatherDisk::Ext::JournalTags;
using FeatherDisk::Ext::JournalTagSameUuid;
using FeatherDisk::Ext::JournalTagSize;
using FeatherDisk::Ext::JournalTagsPerDescriptor;
using FeatherDisk::Ext::JournalTailMatches;
using FeatherDisk::Ext::JournalUuidSize;
using FeatherDisk::Ext::LoadBigEndian32;
using FeatherDisk::Ext::ReadRevokeRecords;
using FeatherDisk::Ext::RevokeRecordsFit;
using FeatherDisk::Ext::RevokeTable;
using FeatherDisk::Ext::StoreBigEndian32;
using FeatherDisk::Tests::Succeeds;

constexpr std::uint32_t Revoke = JournalIncompatibleRevoke;
constexpr std::uint32_t Wide = JournalIncompatible64Bit;
constexpr std::uint32_t V2 = JournalIncompatibleChecksumV2;
constexpr std::uint32_t V3 = JournalIncompatibleChecksumV3;

// Tag sizes of 8, 10, 12, 14, 16 and 16 bytes.
constexpr std::array<std::uint32_t, 6> FeatureSets = {0, V2, Wide, Wide | V2, V3, Wide | V3};

std::array<std::uint8_t, JournalUuidSize> TestUuid()
{
    std::array<std::uint8_t, JournalUuidSize> uuid{};
    for (std::size_t index = 0; index < uuid.size(); ++index)
    {
        uuid[index] = static_cast<std::uint8_t>(0xA0 + index * 3);
    }
    return uuid;
}

std::uint32_t SeedOf(const std::array<std::uint8_t, JournalUuidSize>& uuid)
{
    JournalSuperblock superblock{};
    superblock.uuid = uuid;
    return FeatherDisk::Ext::JournalChecksumSeed(superblock);
}

std::vector<std::uint8_t> PatternBlock(std::uint32_t size, std::uint32_t salt)
{
    std::vector<std::uint8_t> block(size);
    for (std::size_t index = 0; index < block.size(); ++index)
    {
        block[index] = static_cast<std::uint8_t>((index * 31 + salt * 7 + 1) ^ (index >> 8));
    }
    return block;
}

std::uint64_t HomeOf(std::uint32_t features, std::size_t index)
{
    const std::uint64_t high = (features & Wide) != 0 ? 0x1'0000'0000 * (index % 3) : 0;
    return high + 1000 + index * 17;
}

} // namespace

TEST_CASE("jbd2 descriptor blocks written by the encoder read back through the decoder")
{
    const std::array<std::uint8_t, JournalUuidSize> uuid = TestUuid();
    const std::uint32_t seed = SeedOf(uuid);
    constexpr std::uint32_t Sequence = 0x1234ABCD;
    for (const std::uint32_t block_size : {1024U, 4096U})
    {
        for (const std::uint32_t features : FeatureSets)
        {
            INFO("block size " << block_size << " features " << features);
            const bool checksums = JournalHasChecksums(features);
            const std::size_t capacity = JournalTagsPerDescriptor(block_size, features);
            const std::size_t tail = checksums ? 4 : 0;
            REQUIRE(capacity == (block_size - 12 - 32 - tail) / JournalTagSize(features));

            std::vector<std::uint8_t> block(block_size, 0xEE);
            JournalDescriptorWriter writer(block, features, Sequence, uuid);
            std::vector<std::vector<std::uint8_t>> copies;
            for (std::size_t index = 0; index < capacity; ++index)
            {
                std::vector<std::uint8_t> copy =
                    PatternBlock(block_size, static_cast<uint32_t>(index));
                const bool escaped = index % 5 == 2;
                if (escaped)
                {
                    StoreBigEndian32(copy.data(), JournalMagic);
                }
                REQUIRE(EscapeJournalBlock(copy) == escaped);
                REQUIRE(writer.Add(HomeOf(features, index), escaped,
                                   JournalTagChecksum(seed, Sequence, copy)));
                copies.push_back(copy);
            }
            REQUIRE(writer.Count() == capacity);
            REQUIRE_FALSE(writer.Add(5, false, 0));
            REQUIRE(writer.Count() == capacity);
            writer.Finish(seed);

            const FeatherDisk::Ext::JournalHeader header = DecodeJournalHeader(block);
            REQUIRE(header.magic == JournalMagic);
            REQUIRE(header.block_type == JournalDescriptorBlock);
            REQUIRE(header.sequence == Sequence);
            // The UUID follows the first tag, and only it.
            REQUIRE(std::equal(uuid.begin(), uuid.end(),
                               block.begin() + 12 + JournalTagSize(features)));

            JournalTags tags(block, features);
            std::size_t index = 0;
            while (const std::optional<JournalTag> tag = tags.Next())
            {
                REQUIRE(index < capacity);
                REQUIRE(tag->block == HomeOf(features, index));
                const std::uint16_t expected_flags =
                    static_cast<std::uint16_t>((index % 5 == 2 ? JournalTagEscaped : 0) |
                                               (index != 0 ? JournalTagSameUuid : 0) |
                                               (index + 1 == capacity ? JournalTagLast : 0));
                REQUIRE(tag->flags == expected_flags);
                const std::uint32_t full = JournalTagChecksum(seed, Sequence, copies[index]);
                if ((features & V3) != 0)
                {
                    REQUIRE(tag->checksum == full);
                }
                else if ((features & V2) != 0)
                {
                    REQUIRE(tag->checksum == (full & 0xFFFFU));
                }
                else
                {
                    REQUIRE(tag->checksum == 0);
                }
                if (checksums)
                {
                    REQUIRE(JournalTagMatches(*tag, features, seed, Sequence, copies[index]));
                    std::vector<std::uint8_t> flipped = copies[index];
                    flipped[100] ^= 1U;
                    REQUIRE_FALSE(JournalTagMatches(*tag, features, seed, Sequence, flipped));
                    REQUIRE_FALSE(
                        JournalTagMatches(*tag, features, seed, Sequence + 1, copies[index]));
                }
                ++index;
            }
            REQUIRE(index == capacity);

            if (checksums)
            {
                REQUIRE(JournalTailMatches(block, seed));
                for (const std::size_t at : {std::size_t{20}, std::size_t{block_size - 3}})
                {
                    block[at] ^= 0x40U;
                    REQUIRE_FALSE(JournalTailMatches(block, seed));
                    block[at] ^= 0x40U;
                }
                REQUIRE(JournalTailMatches(block, seed));
            }
            else
            {
                // Nothing is stored where a tail would be.
                REQUIRE(std::all_of(block.end() - 4, block.end(),
                                    [](std::uint8_t byte) { return byte == 0; }));
            }
        }
    }
}

TEST_CASE("a descriptor with fewer tags than it holds ends at its last tag")
{
    const std::array<std::uint8_t, JournalUuidSize> uuid = TestUuid();
    for (const std::uint32_t features : FeatureSets)
    {
        std::vector<std::uint8_t> block(1024);
        JournalDescriptorWriter writer(block, features, 9, uuid);
        REQUIRE(writer.Count() == 0);
        for (std::uint64_t home = 1; home <= 3; ++home)
        {
            REQUIRE(writer.Add(home * 100, home == 2, 0));
        }
        writer.Finish(SeedOf(uuid));
        JournalTags tags(block, features);
        std::vector<std::uint64_t> homes;
        while (const std::optional<JournalTag> tag = tags.Next())
        {
            homes.push_back(tag->block);
        }
        REQUIRE(homes == std::vector<std::uint64_t>{100, 200, 300});
    }
}

TEST_CASE("jbd2 revoke blocks written by the encoder read back through the decoder")
{
    const std::array<std::uint8_t, JournalUuidSize> uuid = TestUuid();
    const std::uint32_t seed = SeedOf(uuid);
    constexpr std::uint32_t Sequence = 41;
    for (const std::uint32_t block_size : {1024U, 4096U})
    {
        for (const std::uint32_t features : FeatureSets)
        {
            INFO("block size " << block_size << " features " << features);
            const bool checksums = JournalHasChecksums(features);
            const std::size_t record = (features & Wide) != 0 ? 8 : 4;
            const std::size_t capacity = JournalRevokesPerBlock(block_size, features);
            REQUIRE(capacity == (block_size - 16 - (checksums ? 4 : 0)) / record);

            std::vector<std::uint8_t> block(block_size, 0xEE);
            JournalRevokeWriter writer(block, features, Sequence);
            REQUIRE(writer.Count() == 0);
            for (std::size_t index = 0; index < capacity; ++index)
            {
                REQUIRE(writer.Add(HomeOf(features, index)));
            }
            REQUIRE(writer.Count() == capacity);
            REQUIRE_FALSE(writer.Add(7));
            writer.Finish(seed);

            const FeatherDisk::Ext::JournalHeader header = DecodeJournalHeader(block);
            REQUIRE(header.magic == JournalMagic);
            REQUIRE(header.block_type == JournalRevokeBlock);
            REQUIRE(header.sequence == Sequence);
            REQUIRE(LoadBigEndian32(block.data() + 12) == 16 + capacity * record);
            REQUIRE(RevokeRecordsFit(block, features));
            if (checksums)
            {
                REQUIRE(JournalTailMatches(block, seed));
                block[40] ^= 1U;
                REQUIRE_FALSE(JournalTailMatches(block, seed));
                block[40] ^= 1U;
            }
            RevokeTable table;
            Succeeds(ReadRevokeRecords(block, features, Sequence, table));
            REQUIRE(table.Count() == capacity);
            for (std::size_t index = 0; index < capacity; ++index)
            {
                REQUIRE(table.Revokes(HomeOf(features, index), Sequence));
                REQUIRE_FALSE(table.Revokes(HomeOf(features, index), Sequence + 1));
            }
            REQUIRE_FALSE(table.Revokes(HomeOf(features, capacity), Sequence));
        }
    }
}

TEST_CASE("jbd2 commit blocks written by the encoder read back through the decoder")
{
    const std::array<std::uint8_t, JournalUuidSize> uuid = TestUuid();
    const std::uint32_t seed = SeedOf(uuid);
    for (const std::uint32_t features : FeatureSets)
    {
        INFO("features " << features);
        std::vector<std::uint8_t> block(1024, 0xEE);
        EncodeCommitBlock(block, features, seed, 0x01020304, 0x0000'0001'6000'0001, 999'999'999);
        const FeatherDisk::Ext::JournalHeader header = DecodeJournalHeader(block);
        REQUIRE(header.magic == JournalMagic);
        REQUIRE(header.block_type == JournalCommitBlock);
        REQUIRE(header.sequence == 0x01020304);
        REQUIRE(LoadBigEndian32(block.data() + 48) == 1);
        REQUIRE(LoadBigEndian32(block.data() + 52) == 0x6000'0001);
        REQUIRE(LoadBigEndian32(block.data() + 56) == 999'999'999);
        for (std::size_t index = 12; index < block.size(); ++index)
        {
            const bool stored = (index >= 16 && index < 20 && JournalHasChecksums(features)) ||
                                (index >= 48 && index < 60);
            if (!stored)
            {
                REQUIRE(block[index] == 0);
            }
        }
        if (JournalHasChecksums(features))
        {
            REQUIRE(JournalCommitMatches(block, seed));
            for (const std::size_t at : {std::size_t{14}, std::size_t{60}, std::size_t{1023}})
            {
                block[at] ^= 1U;
                REQUIRE_FALSE(JournalCommitMatches(block, seed));
                block[at] ^= 1U;
            }
        }
    }
}

TEST_CASE("escaping touches only blocks that begin with the journal's magic")
{
    std::vector<std::uint8_t> block = PatternBlock(1024, 3);
    std::vector<std::uint8_t> original = block;
    REQUIRE_FALSE(EscapeJournalBlock(block));
    REQUIRE(block == original);

    // Little-endian, one byte off, and the magic one byte in are not it.
    StoreBigEndian32(block.data(), JournalMagic);
    block[3] = 0x99;
    original = block;
    REQUIRE_FALSE(EscapeJournalBlock(block));
    REQUIRE(block == original);
    StoreBigEndian32(block.data(), 0x9839'3BC0);
    original = block;
    REQUIRE_FALSE(EscapeJournalBlock(block));
    REQUIRE(block == original);
    block = PatternBlock(1024, 4);
    StoreBigEndian32(block.data() + 1, JournalMagic);
    original = block;
    REQUIRE_FALSE(EscapeJournalBlock(block));
    REQUIRE(block == original);

    block = PatternBlock(1024, 5);
    StoreBigEndian32(block.data(), JournalMagic);
    original = block;
    REQUIRE(EscapeJournalBlock(block));
    REQUIRE(
        std::all_of(block.begin(), block.begin() + 4, [](std::uint8_t byte) { return byte == 0; }));
    REQUIRE(std::equal(block.begin() + 4, block.end(), original.begin() + 4));
    // The checksum covers the escaped copy, not the block.
    const std::uint32_t seed = SeedOf(TestUuid());
    REQUIRE(JournalTagChecksum(seed, 1, block) != JournalTagChecksum(seed, 1, original));
}

TEST_CASE("journal features written by the encoder pass the decoder and the check")
{
    const std::array<std::uint8_t, JournalUuidSize> uuid = TestUuid();
    for (const std::uint32_t features : FeatureSets)
    {
        for (const std::uint32_t compatible : {0U, JournalCompatibleChecksum})
        {
            INFO("features " << features << " compatible " << compatible);
            std::vector<std::uint8_t> block(1024);
            StoreBigEndian32(block.data(), JournalMagic);
            StoreBigEndian32(block.data() + 4, JournalSuperblockVersion2);
            StoreBigEndian32(block.data() + 12, 1024);
            StoreBigEndian32(block.data() + 16, 1024);
            StoreBigEndian32(block.data() + 20, 1);
            std::copy(uuid.begin(), uuid.end(), block.begin() + 48);
            JournalSuperblock superblock = DecodeJournalSuperblock(block);
            superblock.compatible = compatible;
            superblock.incompatible = Revoke | features;
            superblock.sequence = 77;
            superblock.start = 5;
            EncodeJournalFeatures(superblock, block);
            EncodeJournalPosition(superblock, block);

            const JournalSuperblock decoded = DecodeJournalSuperblock(block);
            REQUIRE(decoded.compatible == compatible);
            REQUIRE(decoded.incompatible == (Revoke | features));
            REQUIRE(decoded.sequence == 77);
            REQUIRE(decoded.start == 5);
            REQUIRE(decoded.checksum_matches);
            REQUIRE(decoded.checksum_type == (JournalHasChecksums(features) ? 4 : 0));
            Succeeds(CheckJournalSuperblock(decoded, 1024, 1024));
            if (JournalHasChecksums(features))
            {
                block[200] ^= 1U;
                REQUIRE_FALSE(DecodeJournalSuperblock(block).checksum_matches);
            }
        }
    }
    // Version 1 superblocks have no features.
    std::vector<std::uint8_t> version1(1024);
    StoreBigEndian32(version1.data(), JournalMagic);
    StoreBigEndian32(version1.data() + 4, FeatherDisk::Ext::JournalSuperblockVersion1);
    StoreBigEndian32(version1.data() + 36, 0xFFFF);
    REQUIRE(DecodeJournalSuperblock(version1).compatible == 0);
}

#ifdef FEATHERDISK_EXT_IMAGES
namespace
{

using FeatherDisk::NodeId;
using FeatherDisk::Ext::Ext2Type;
using FeatherDisk::Tests::BytesOf;
using FeatherDisk::Tests::ChangeWithDebugfs;
using FeatherDisk::Tests::CheckClean;
using FeatherDisk::Tests::CopyOf;
using FeatherDisk::Tests::DeviceFrom;
using FeatherDisk::Tests::JournalOffset;
using FeatherDisk::Tests::JournalSuperblockOf;
using FeatherDisk::Tests::LoadExtImage;
using FeatherDisk::Tests::MemoryDevice;
using FeatherDisk::Tests::MountedFilesystem;
using FeatherDisk::Tests::RawVolume;
using FeatherDisk::Tests::ReadAll;
using FeatherDisk::Tests::ReplayByMounting;
using FeatherDisk::Tests::RequireSameAsDebugfs;
using FeatherDisk::Tests::RunDebugfs;
using FeatherDisk::Tests::Value;
using FeatherDisk::Tests::Walk;

constexpr std::uint64_t SuperblockStart = 1024;
constexpr std::uint32_t NeedsRecovery = 0x0004;
constexpr std::uint32_t MetadataChecksum = 0x0400;

struct WrittenCase
{
    std::string_view image;
    std::uint32_t incompatible;
    std::size_t tag_size;
};

constexpr std::array<WrittenCase, 6> WrittenCases = {{
    {.image = "ext3", .incompatible = Revoke, .tag_size = 8},
    {.image = "ext4-blockmap", .incompatible = Revoke | V2, .tag_size = 10},
    {.image = "ext4-journal-gdtcsum", .incompatible = Revoke | Wide, .tag_size = 12},
    {.image = "ext4-64bit", .incompatible = Revoke | Wide | V2, .tag_size = 14},
    {.image = "ext4", .incompatible = Revoke | Wide | V3, .tag_size = 16},
    {.image = "ext4-blockmap", .incompatible = Revoke | V3, .tag_size = 16},
}};

struct Copy
{
    std::uint64_t home;
    std::vector<std::uint8_t> bytes;
};

// Writes blocks into the ring from a position, wrapping from s_maxlen - 1 to s_first.
class LogWriter
{
public:
    LogWriter(std::vector<std::uint8_t>& image, const JournalSuperblock& superblock,
              std::uint32_t position)
        : image(image), raw{image}, superblock(superblock),
          seed(FeatherDisk::Ext::JournalChecksumSeed(superblock)), position(position)
    {
    }

    void Put(std::span<const std::uint8_t> block)
    {
        const std::uint64_t at = JournalOffset(raw, position);
        std::copy(block.begin(), block.end(), image.begin() + static_cast<std::ptrdiff_t>(at));
        position = position + 1 == superblock.maximum_length ? superblock.first : position + 1;
    }

    void Transaction(std::uint32_t sequence, const std::vector<std::uint64_t>& revokes,
                     const std::vector<Copy>& copies, bool commit)
    {
        const std::uint32_t size = raw.BlockSize();
        const std::uint32_t features = superblock.incompatible;
        std::vector<std::uint8_t> block(size);
        if (!revokes.empty())
        {
            JournalRevokeWriter writer(block, features, sequence);
            for (const std::uint64_t revoked : revokes)
            {
                REQUIRE(writer.Add(revoked));
            }
            writer.Finish(seed);
            Put(block);
        }
        if (!copies.empty())
        {
            JournalDescriptorWriter writer(block, features, sequence, superblock.uuid);
            std::vector<std::vector<std::uint8_t>> logged;
            for (const Copy& copy : copies)
            {
                std::vector<std::uint8_t> bytes = copy.bytes;
                const bool escaped = EscapeJournalBlock(bytes);
                REQUIRE(writer.Add(copy.home, escaped, JournalTagChecksum(seed, sequence, bytes)));
                logged.push_back(std::move(bytes));
            }
            writer.Finish(seed);
            Put(block);
            for (const std::vector<std::uint8_t>& bytes : logged)
            {
                Put(bytes);
            }
        }
        if (commit)
        {
            EncodeCommitBlock(block, features, seed, sequence, 1'700'000'000, 123'456'789);
            Put(block);
        }
    }

private:
    std::vector<std::uint8_t>& image;
    RawVolume raw;
    JournalSuperblock superblock;
    std::uint32_t seed;
    std::uint32_t position;
};

struct BuiltLog
{
    std::unique_ptr<MemoryDevice> device;
    std::vector<std::uint8_t> original;
    std::uint32_t block_size;
    std::uint32_t first;
    std::uint64_t hello_block;
    std::array<std::uint64_t, 3> spare;
    std::vector<std::uint8_t> escaped;
    std::vector<std::uint8_t> later;
    std::string logdump;
};

std::vector<std::uint8_t> Text(std::uint32_t size, std::string_view text)
{
    std::vector<std::uint8_t> block(size);
    std::copy(text.begin(), text.end(), block.begin());
    return block;
}

// Two committed transactions and a torn one, written from three blocks before the ring's end:
//   first:  hello.txt's inode table block with mode 0600, spare[0] led by the magic, and spare[1]
//   second: a revoke of spare[1], and spare[2]
//   third:  spare[0] again, without its commit block
BuiltLog BuildLog(const WrittenCase& written)
{
    BuiltLog built{};
    std::vector<std::uint8_t> image = LoadExtImage(written.image);
    built.original = image;
    RawVolume raw{image};
    built.block_size = raw.BlockSize();
    const std::uint32_t size = built.block_size;
    const std::unique_ptr<MemoryDevice> plain = DeviceFrom(image);
    const std::uint32_t hello = raw.Load32(raw.EntryOffset(2, "hello.txt"));
    built.hello_block = raw.FileBlock(hello, 0);
    const std::uint64_t inode_block = raw.InodeOffset(hello) / size;
    const std::unique_ptr<MemoryDevice> changed = CopyOf(*plain);
    REQUIRE(
        ChangeWithDebugfs(*changed, "journal-write-sif", "sif /hello.txt mode 0100600").status ==
        0);
    const std::span<const std::uint8_t> inode_table =
        changed->Bytes().subspan(inode_block * size, size);

    const std::uint64_t last = raw.BlockCount() - 8;
    built.spare = {last, last + 1, last + 2};
    const std::string tested =
        RunDebugfs(*plain, "journal-write-testb", "testb " + std::to_string(last) + " 3").output;
    REQUIRE(std::count(tested.begin(), tested.end(), '\n') == 3);
    REQUIRE(tested.find(" marked in use") == std::string::npos);

    JournalSuperblock superblock = JournalSuperblockOf(*plain);
    REQUIRE(superblock.header.block_type == JournalSuperblockVersion2);
    built.first = superblock.sequence;
    superblock.compatible = 0;
    superblock.incompatible = written.incompatible;
    superblock.start = superblock.maximum_length - 3;
    const std::uint64_t journal = JournalOffset(raw, 0);
    const std::span<std::uint8_t> journal_block(image.data() + journal, size);
    EncodeJournalFeatures(superblock, journal_block);
    EncodeJournalPosition(superblock, journal_block);

    built.escaped = Text(size, "....an escaped block");
    StoreBigEndian32(built.escaped.data(), JournalMagic);
    built.later = Text(size, "committed in the second transaction");
    LogWriter log(image, superblock, superblock.start);
    log.Transaction(
        built.first, {},
        {{inode_block, std::vector<std::uint8_t>(inode_table.begin(), inode_table.end())},
         {built.spare[0], built.escaped},
         {built.spare[1], Text(size, "revoked by the second transaction")}},
        true);
    log.Transaction(built.first + 1, {built.spare[1]}, {{built.spare[2], built.later}}, true);
    log.Transaction(built.first + 2, {}, {{built.spare[0], Text(size, "never committed")}}, false);

    raw.Store32(SuperblockStart + 96, raw.Load32(SuperblockStart + 96) | NeedsRecovery);
    if ((raw.Load32(SuperblockStart + 100) & MetadataChecksum) != 0)
    {
        raw.UpdateSuperblockChecksum();
    }
    built.original = image;
    built.device = DeviceFrom(image);
    built.logdump = RunDebugfs(*built.device, "journal-write-logdump", "logdump -a").output;
    return built;
}

std::size_t Count(const std::string& text, std::string_view needle)
{
    std::size_t count = 0;
    for (std::size_t at = text.find(needle); at != std::string::npos;
         at = text.find(needle, at + 1))
    {
        ++count;
    }
    return count;
}

std::string BlockPrefix(MemoryDevice& device, std::uint64_t block, std::uint32_t block_size)
{
    const std::span<const std::uint8_t> bytes =
        device.Bytes().subspan(block * block_size, block_size);
    return {bytes.begin(), std::find(bytes.begin(), bytes.end(), std::uint8_t{0})};
}

bool SameAs(MemoryDevice& device, const std::vector<std::uint8_t>& other, std::uint64_t block,
            std::uint32_t block_size)
{
    return std::equal(other.begin() + static_cast<std::ptrdiff_t>(block * block_size),
                      other.begin() + static_cast<std::ptrdiff_t>((block + 1) * block_size),
                      device.Bytes().begin() + static_cast<std::ptrdiff_t>(block * block_size));
}

} // namespace
#endif

TEST_CASE("logs built by the encoders are read by debugfs and replayed as it replays them")
{
#ifndef FEATHERDISK_EXT_IMAGES
    SKIP("e2fsprogs is not installed");
#else
    for (const WrittenCase& written : WrittenCases)
    {
        INFO(written.image << " with " << written.tag_size << "-byte tags");
        REQUIRE(JournalTagSize(written.incompatible) == written.tag_size);
        BuiltLog built = BuildLog(written);
        const std::uint32_t size = built.block_size;

        // logdump lists exactly the blocks written: two transactions with a commit block each, the
        // torn third with none, and the revoke.
        const std::string& dump = built.logdump;
        INFO(dump);
        REQUIRE(Count(dump, "type 1 (descriptor block)") == 3);
        REQUIRE(Count(dump, "type 2 (commit block)") == 2);
        REQUIRE(Count(dump, "type 5 (revoke table)") == 1);
        REQUIRE(Count(dump, " logged at journal block ") == 5);
        REQUIRE(Count(dump, "FS block " + std::to_string(built.spare[0]) + " logged") == 2);
        REQUIRE(Count(dump, "FS block " + std::to_string(built.spare[1]) + " logged") == 1);
        REQUIRE(Count(dump, "FS block " + std::to_string(built.spare[2]) + " logged") == 1);
        REQUIRE(Count(dump, "Revoke FS block " + std::to_string(built.spare[1])) == 1);
        // Only the second tag, the escaped copy, carries ESCAPE with SAME_UUID.
        REQUIRE(Count(dump, "(flags 0x3)") == 1);

        // No block in the ring but the three descriptors, the two commit blocks and the revoke
        // block begins with the magic, so the escaped copy was logged with zeros in its place.
        {
            std::vector<std::uint8_t> bytes = BytesOf(*built.device);
            const RawVolume raw{bytes};
            const JournalSuperblock journal = JournalSuperblockOf(*built.device);
            std::size_t magic = 0;
            for (std::uint32_t at = journal.first; at < journal.maximum_length; ++at)
            {
                magic += LoadBigEndian32(bytes.data() + JournalOffset(raw, at)) == JournalMagic;
            }
            REQUIRE(magic == 6);
            const std::string marker =
                "FS block " + std::to_string(built.spare[0]) + " logged at journal block ";
            const std::size_t line = dump.find(marker);
            REQUIRE(line != std::string::npos);
            const auto copy =
                static_cast<std::uint32_t>(std::stoul(dump.substr(line + marker.size())));
            const std::uint64_t offset = JournalOffset(raw, copy);
            REQUIRE(LoadBigEndian32(bytes.data() + offset) == 0);
            REQUIRE(std::equal(built.escaped.begin() + 4, built.escaped.end(),
                               bytes.begin() + static_cast<std::ptrdiff_t>(offset + 4)));
        }

        const std::unique_ptr<MemoryDevice> expected = CopyOf(*built.device);
        REQUIRE(ChangeWithDebugfs(*expected, "journal-write-run", "jr").status == 0);
        ReplayByMounting(*built.device);
        RequireSameAsDebugfs(*built.device, *expected);
        CheckClean(*built.device, "journal-write-replayed");
        CheckClean(*expected, "journal-write-debugfs");

        const JournalSuperblock after = JournalSuperblockOf(*built.device);
        REQUIRE(after.start == 0);
        REQUIRE(after.sequence == built.first + 3);
        REQUIRE(std::equal(built.escaped.begin(), built.escaped.end(),
                           built.device->Bytes().begin() +
                               static_cast<std::ptrdiff_t>(built.spare[0] * size)));
        REQUIRE(SameAs(*built.device, built.original, built.spare[1], size));
        REQUIRE(BlockPrefix(*built.device, built.spare[2], size) ==
                "committed in the second transaction");
        MountedFilesystem ext(Ext2Type(), *built.device, true);
        const NodeId hello = Value(Walk(*ext, "hello.txt"));
        REQUIRE(Value(ext->GetAttributes(hello)).mode == 0600);
        REQUIRE(ReadAll(*ext, hello) == "Hello from ext2.\n");
    }
#endif
}
