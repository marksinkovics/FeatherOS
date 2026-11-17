// SPDX-License-Identifier: MIT OR Apache-2.0
// jbd2 journal recovery: the format's pieces on built blocks, then journals that debugfs, an
// independent implementation, wrote into the images of tests/make_ext2_image.sh (jo, jw, jc)
// without replaying them. A writable mount must leave each volume as debugfs's own replay (jr)
// does, with e2fsck -fn clean; damaged journals end early or are refused before any write.
#include <catch2/catch_test_macros.hpp>

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
#include <vector>

namespace
{

using FeatherDisk::Error;
using FeatherDisk::Ext::JournalIncompatible64Bit;
using FeatherDisk::Ext::JournalIncompatibleChecksumV2;
using FeatherDisk::Ext::JournalIncompatibleChecksumV3;
using FeatherDisk::Ext::JournalMagic;
using FeatherDisk::Ext::JournalTag;
using FeatherDisk::Ext::JournalTags;
using FeatherDisk::Ext::JournalTagSize;
using FeatherDisk::Ext::RevokeTable;
using FeatherDisk::Ext::StoreBigEndian32;
using FeatherDisk::Tests::Succeeds;

std::vector<std::uint8_t> Descriptor(std::size_t size)
{
    std::vector<std::uint8_t> block(size);
    StoreBigEndian32(block.data(), JournalMagic);
    StoreBigEndian32(block.data() + 4, 1);
    StoreBigEndian32(block.data() + 8, 7);
    return block;
}

} // namespace

TEST_CASE("jbd2 tags are 8 to 16 bytes by the journal's features")
{
    REQUIRE(JournalTagSize(0) == 8);
    REQUIRE(JournalTagSize(JournalIncompatibleChecksumV2) == 10);
    REQUIRE(JournalTagSize(JournalIncompatible64Bit) == 12);
    REQUIRE(JournalTagSize(JournalIncompatible64Bit | JournalIncompatibleChecksumV2) == 14);
    REQUIRE(JournalTagSize(JournalIncompatibleChecksumV3) == 16);
    REQUIRE(JournalTagSize(JournalIncompatible64Bit | JournalIncompatibleChecksumV3) == 16);
}

TEST_CASE("jbd2 descriptor tags carry 64-bit blocks, UUIDs and a last flag")
{
    // 64-bit tags without checksums: block 0x1_0000_0005 with a UUID, block 9 without, block 11
    // last; the tag after it is never read.
    std::vector<std::uint8_t> block = Descriptor(1024);
    std::size_t at = 12;
    const auto tag = [&](std::uint32_t low, std::uint32_t high, std::uint8_t flags)
    {
        StoreBigEndian32(block.data() + at, low);
        block[at + 7] = flags;
        StoreBigEndian32(block.data() + at + 8, high);
        at += 12 + ((flags & 2) == 0 ? 16 : 0);
    };
    tag(5, 1, 0);
    tag(9, 0, 2);
    tag(11, 0, 2 | 8);
    tag(13, 0, 2);
    JournalTags tags(block, JournalIncompatible64Bit);
    std::vector<std::uint64_t> blocks;
    while (const std::optional<JournalTag> next = tags.Next())
    {
        blocks.push_back(next->block);
    }
    REQUIRE(blocks == std::vector<std::uint64_t>{0x100000005, 9, 11});

    // Without a last flag the tags end where another would not fit before the checksum tail:
    // (1024 - 12 - 4) / 16 v3 tags, each with the same UUID.
    block = Descriptor(1024);
    for (std::size_t offset = 12; offset + 16 <= 1024; offset += 16)
    {
        StoreBigEndian32(block.data() + offset, static_cast<std::uint32_t>(offset));
        block[offset + 7] = 2;
    }
    JournalTags v3(block, JournalIncompatibleChecksumV3);
    std::size_t count = 0;
    while (v3.Next())
    {
        ++count;
    }
    REQUIRE(count == (1024 - 12 - 4) / 16);
}

TEST_CASE("jbd2 revoke records keep the latest transaction that revoked each block")
{
    RevokeTable table;
    REQUIRE_FALSE(table.Revokes(5, 1));
    for (std::uint64_t block = 0; block < 10000; ++block)
    {
        Succeeds(table.Add(block * 7919, 10));
    }
    Succeeds(table.Add(7919, 12));
    Succeeds(table.Add(7919, 11));
    REQUIRE(table.Count() == 10000);
    // A copy in the revoking transaction or before it is revoked; a later one is not.
    REQUIRE(table.Revokes(0, 10));
    REQUIRE(table.Revokes(0, 3));
    REQUIRE_FALSE(table.Revokes(0, 11));
    REQUIRE(table.Revokes(7919, 12));
    REQUIRE_FALSE(table.Revokes(7919, 13));
    REQUIRE_FALSE(table.Revokes(7920, 1));
    // Sequences wrap: 2 comes after 0xFFFFFFF0.
    Succeeds(table.Add(1, 0xFFFFFFF0));
    REQUIRE(table.Revokes(1, 0xFFFFFFF0));
    REQUIRE_FALSE(table.Revokes(1, 2));
}

#ifdef FEATHERDISK_EXT_IMAGES
namespace
{

using FeatherDisk::NodeId;
using FeatherDisk::Ext::DecodeJournalHeader;
using FeatherDisk::Ext::DecodeJournalSuperblock;
using FeatherDisk::Ext::Ext2Type;
using FeatherDisk::Ext::JournalCommitBlock;
using FeatherDisk::Ext::JournalDescriptorBlock;
using FeatherDisk::Ext::JournalHeader;
using FeatherDisk::Ext::JournalRevokeBlock;
using FeatherDisk::Ext::JournalSuperblock;
using FeatherDisk::Ext::LoadBigEndian32;
using FeatherDisk::Tests::BytesOf;
using FeatherDisk::Tests::ChangeWithDebugfs;
using FeatherDisk::Tests::CheckClean;
using FeatherDisk::Tests::CopyOf;
using FeatherDisk::Tests::DeviceFrom;
using FeatherDisk::Tests::ErrorOf;
using FeatherDisk::Tests::LoadExtImage;
using FeatherDisk::Tests::MemoryDevice;
using FeatherDisk::Tests::MountedFilesystem;
using FeatherDisk::Tests::RawVolume;
using FeatherDisk::Tests::ReadAll;
using FeatherDisk::Tests::RunDebugfs;
using FeatherDisk::Tests::ScriptWithDebugfs;
using FeatherDisk::Tests::Value;
using FeatherDisk::Tests::Walk;

constexpr std::uint64_t SuperblockStart = 1024;
constexpr std::uint32_t NeedsRecovery = 0x0004;
constexpr std::string_view OldHello = "Hello from ext2.\n";
constexpr std::string_view NewHello = "Journal replayed\n";

struct JournalCase
{
    std::string_view image;
    // debugfs's journal_open, with the checksum options.
    std::string_view open;
};

// 8, 10, 12, 14 and 16-byte tags between them. debugfs gives checksums v2 and v3 only to volumes
// with metadata_csum; on ext3, -c means the compatible v1 checksums, which recovery ignores.
constexpr std::array<JournalCase, 6> Cases = {{
    {.image = "ext3", .open = "jo"},
    {.image = "ext4-blockmap", .open = "jo -c -v 2"},
    {.image = "ext4", .open = "jo"},
    {.image = "ext4", .open = "jo -c"},
    {.image = "ext4-64bit", .open = "jo -c -v 2"},
    {.image = "ext4-blockmap", .open = "jo -c"},
}};

JournalSuperblock JournalSuperblockOf(MemoryDevice& device);

std::vector<std::uint8_t> TextBlock(std::uint32_t size, std::string_view text)
{
    std::vector<std::uint8_t> block(size);
    std::copy(text.begin(), text.end(), block.begin());
    return block;
}

// A volume with a journal that needs recovery: transaction 1 gives hello.txt new contents and mode
// 0600 (its inode table block from a copy that debugfs changed), 2 writes three spare blocks, the
// first starting with the journal's magic (so escaped), 3 revokes the second and third, 4 writes
// the second again, and 5 writes the third without a commit.
struct Journalled
{
    std::unique_ptr<MemoryDevice> device;
    std::vector<std::uint8_t> original;
    std::uint32_t block_size;
    std::uint64_t hello_block;
    std::array<std::uint64_t, 3> spare;
    std::vector<std::uint8_t> escaped;
    std::vector<std::uint8_t> rewritten;
    // The sequence of transaction 1.
    std::uint32_t first;
};

Journalled MakeJournalled(const JournalCase& journal_case)
{
    Journalled made{};
    made.original = LoadExtImage(journal_case.image);
    made.device = DeviceFrom(made.original);
    RawVolume raw{made.original};
    made.block_size = raw.BlockSize();
    std::uint32_t hello = 0;
    {
        MountedFilesystem ext(Ext2Type(), *made.device, true);
        hello = static_cast<std::uint32_t>(Value(Walk(*ext, "hello.txt")));
    }
    made.hello_block = raw.FileBlock(hello, 0);
    const std::uint64_t inode_block = raw.InodeOffset(hello) / made.block_size;
    const std::unique_ptr<MemoryDevice> changed = CopyOf(*made.device);
    REQUIRE(ChangeWithDebugfs(*changed, "journal-sif", "sif /hello.txt mode 0100600").status == 0);
    const std::span<const std::uint8_t> inode_table =
        changed->Bytes().subspan(inode_block * made.block_size, made.block_size);

    const std::uint64_t last = raw.BlockCount() - 8;
    made.spare = {last, last + 1, last + 2};
    const std::string tested =
        RunDebugfs(*made.device, "journal-testb", "testb " + std::to_string(last) + " 3").output;
    REQUIRE(std::count(tested.begin(), tested.end(), '\n') == 3);
    REQUIRE(tested.find(" marked in use") == std::string::npos);

    std::vector<std::uint8_t> first(inode_table.begin(), inode_table.end());
    const std::vector<std::uint8_t> hello_data = TextBlock(made.block_size, NewHello);
    first.insert(first.end(), hello_data.begin(), hello_data.end());
    made.escaped = TextBlock(made.block_size, "....an escaped block");
    StoreBigEndian32(made.escaped.data(), JournalMagic);
    std::vector<std::uint8_t> second = made.escaped;
    for (const std::string_view text : {"revoked copy", "revoked and never written"})
    {
        const std::vector<std::uint8_t> block = TextBlock(made.block_size, text);
        second.insert(second.end(), block.begin(), block.end());
    }
    made.rewritten = TextBlock(made.block_size, "written again after its revoke");
    const std::vector<std::uint8_t> torn = TextBlock(made.block_size, "never committed");

    const auto number = [](std::uint64_t block) { return std::to_string(block); };
    const std::string script =
        std::string(journal_case.open) + "\n" + "jw -b " + number(inode_block) + "," +
        number(made.hello_block) + " %0\n" + "jw -b " + number(made.spare[0]) + "," +
        number(made.spare[1]) + "," + number(made.spare[2]) + " %1\n" + "jw -r " +
        number(made.spare[1]) + "," + number(made.spare[2]) + "\n" + "jw -b " +
        number(made.spare[1]) + " %2\n" + "jw -b " + number(made.spare[2]) + " -c %3\n" + "jc\n";
    REQUIRE(ScriptWithDebugfs(*made.device, "journal-write", script,
                              {first, second, made.rewritten, torn})
                .status == 0);
    RawVolume journalled{made.original};
    std::copy(made.device->Bytes().begin(), made.device->Bytes().end(), made.original.begin());
    REQUIRE((journalled.Load32(SuperblockStart + 96) & NeedsRecovery) != 0);
    made.first = JournalSuperblockOf(*made.device).sequence;
    return made;
}

std::uint32_t JournalInode(const RawVolume& raw)
{
    return raw.Load32(SuperblockStart + 224);
}

// Byte offset of the journal's block n: through the extent tree, or a block map's direct and
// single indirect blocks.
std::uint64_t JournalOffset(const RawVolume& raw, std::uint32_t n)
{
    const std::uint32_t inode = JournalInode(raw);
    if (n < 12 || (raw.Load32(raw.InodeOffset(inode) + 32) & 0x80000U) != 0)
    {
        return raw.FileBlock(inode, n) * raw.BlockSize();
    }
    REQUIRE(n - 12 < raw.BlockSize() / 4);
    return raw.BlockOffset(raw.Load32(raw.BlockOffset(raw.InodeBlock(inode, 12)) + (n - 12) * 4));
}

JournalSuperblock JournalSuperblockOf(MemoryDevice& device)
{
    std::vector<std::uint8_t> bytes = BytesOf(device);
    const RawVolume raw{bytes};
    return DecodeJournalSuperblock(std::span(bytes).subspan(JournalOffset(raw, 0), 1024));
}

struct LogBlock
{
    std::uint32_t position;
    JournalHeader header;
};

// The blocks of the log with a header, in order, from s_start to the first without one.
std::vector<LogBlock> LogBlocks(std::vector<std::uint8_t>& bytes)
{
    const RawVolume raw{bytes};
    const JournalSuperblock superblock =
        DecodeJournalSuperblock(std::span(bytes).subspan(JournalOffset(raw, 0), 1024));
    std::vector<LogBlock> blocks;
    std::uint32_t position = superblock.start;
    for (;;)
    {
        const std::span<const std::uint8_t> block =
            std::span(bytes).subspan(JournalOffset(raw, position), raw.BlockSize());
        const JournalHeader header = DecodeJournalHeader(block);
        if (header.magic != JournalMagic)
        {
            return blocks;
        }
        blocks.push_back({.position = position, .header = header});
        ++position;
        if (header.block_type == JournalDescriptorBlock)
        {
            JournalTags tags(block, superblock.incompatible);
            while (tags.Next())
            {
                ++position;
            }
        }
    }
}

std::uint32_t FindLogBlock(std::vector<std::uint8_t>& bytes, std::uint32_t sequence,
                           std::uint32_t type)
{
    for (const LogBlock& block : LogBlocks(bytes))
    {
        if (block.header.sequence == sequence && block.header.block_type == type)
        {
            return block.position;
        }
    }
    FAIL("no block of type " << type << " in transaction " << sequence);
    return 0;
}

std::string BlockText(MemoryDevice& device, std::uint64_t block, std::uint32_t block_size)
{
    const std::span<const std::uint8_t> bytes =
        device.Bytes().subspan(block * block_size, block_size);
    const auto end = std::find(bytes.begin(), bytes.end(), std::uint8_t{0});
    return {bytes.begin(), end};
}

bool SameBlock(MemoryDevice& device, const std::vector<std::uint8_t>& other, std::uint64_t block,
               std::uint32_t block_size)
{
    return std::equal(other.begin() + static_cast<std::ptrdiff_t>(block * block_size),
                      other.begin() + static_cast<std::ptrdiff_t>((block + 1) * block_size),
                      device.Bytes().begin() + static_cast<std::ptrdiff_t>(block * block_size));
}

void RequireSameBytes(std::vector<std::uint8_t> ours, std::vector<std::uint8_t> theirs)
{
    REQUIRE(ours.size() == theirs.size());
    const auto difference = std::mismatch(ours.begin(), ours.end(), theirs.begin());
    INFO("first difference at byte " << (difference.first - ours.begin()));
    REQUIRE(difference.first == ours.end());
}

// debugfs's journal_run also writes s_wtime, s_lastcheck, s_kbytes_written and their high bytes,
// and the superblock checksum over them; everything else must match.
void RequireSameAsDebugfs(MemoryDevice& ours, MemoryDevice& theirs)
{
    std::vector<std::uint8_t> left = BytesOf(ours);
    std::vector<std::uint8_t> right = BytesOf(theirs);
    constexpr std::array<std::pair<std::uint64_t, std::uint64_t>, 5> Written = {{
        {48, 4},
        {64, 4},
        {376, 8},
        {0x274, 4},
        {1020, 4},
    }};
    for (const auto& [offset, length] : Written)
    {
        for (std::uint64_t index = 0; index < length; ++index)
        {
            left[SuperblockStart + offset + index] = 0;
            right[SuperblockStart + offset + index] = 0;
        }
    }
    RequireSameBytes(std::move(left), std::move(right));
}

// Replays by mounting for writing; the recovered volume mounts read-only, since writes through
// the journal come later.
void Replay(MemoryDevice& device)
{
    MountedFilesystem ext(Ext2Type(), device, false);
    REQUIRE_FALSE(ext->GetCapabilities().writable);
}

void CheckEmptyJournal(MemoryDevice& device, std::uint32_t sequence)
{
    std::vector<std::uint8_t> bytes = BytesOf(device);
    const RawVolume raw{bytes};
    REQUIRE((raw.Load32(SuperblockStart + 96) & NeedsRecovery) == 0);
    const JournalSuperblock journal = JournalSuperblockOf(device);
    REQUIRE(journal.start == 0);
    REQUIRE(journal.sequence == sequence);
    REQUIRE(journal.checksum_matches);
}

void CheckUnchanged(MemoryDevice& device, const std::vector<std::uint8_t>& original)
{
    RequireSameBytes(BytesOf(device), original);
}

} // namespace
#endif

TEST_CASE("a writable mount replays committed jbd2 transactions as debugfs does")
{
#ifndef FEATHERDISK_EXT_IMAGES
    SKIP("e2fsprogs is not installed");
#else
    for (const JournalCase& journal_case : Cases)
    {
        INFO(journal_case.image << " " << journal_case.open);
        Journalled made = MakeJournalled(journal_case);
        const std::unique_ptr<MemoryDevice> expected = CopyOf(*made.device);
        REQUIRE(ChangeWithDebugfs(*expected, "journal-run", "jr").status == 0);

        Replay(*made.device);
        RequireSameAsDebugfs(*made.device, *expected);
        CheckClean(*made.device, "journal-replayed");
        // Transactions 1 to 4 were replayed, so the journal waits for 6: 5 never committed.
        CheckEmptyJournal(*made.device, made.first + 5);
        const std::uint32_t size = made.block_size;
        REQUIRE(std::equal(made.escaped.begin(), made.escaped.end(),
                           made.device->Bytes().begin() +
                               static_cast<std::ptrdiff_t>(made.spare[0] * size)));
        REQUIRE(BlockText(*made.device, made.spare[1], size) == "written again after its revoke");
        REQUIRE(SameBlock(*made.device, made.original, made.spare[2], size));
        {
            MountedFilesystem ext(Ext2Type(), *made.device, true);
            const NodeId hello = Value(Walk(*ext, "hello.txt"));
            REQUIRE(ReadAll(*ext, hello) == NewHello);
            REQUIRE(Value(ext->GetAttributes(hello)).mode == 0600);
        }
        // Clean now: another writable mount changes nothing.
        const std::vector<std::uint8_t> recovered = BytesOf(*made.device);
        Replay(*made.device);
        CheckUnchanged(*made.device, recovered);
    }
#endif
}

TEST_CASE("a read-only mount reads a volume whose journal needs recovery as it is on disk")
{
#ifndef FEATHERDISK_EXT_IMAGES
    SKIP("e2fsprogs is not installed");
#else
    for (const JournalCase& journal_case : {Cases[0], Cases[3]})
    {
        INFO(journal_case.image << " " << journal_case.open);
        Journalled made = MakeJournalled(journal_case);
        {
            MountedFilesystem ext(Ext2Type(), *made.device, true);
            REQUIRE(ReadAll(*ext, Value(Walk(*ext, "hello.txt"))) == OldHello);
        }
        CheckUnchanged(*made.device, made.original);
        REQUIRE(made.device->flush_count == 0);

        // A device that refuses writes mounts read-only the same way.
        made.device->read_only = true;
        Replay(*made.device);
        CheckUnchanged(*made.device, made.original);
    }
#endif
}

TEST_CASE("a jbd2 checksum that does not match in a committed transaction refuses recovery")
{
#ifndef FEATHERDISK_EXT_IMAGES
    SKIP("e2fsprogs is not installed");
#else
    for (const JournalCase& journal_case : {Cases[1], Cases[3], Cases[4], Cases[5]})
    {
        INFO(journal_case.image << " " << journal_case.open);
        // Transaction 2's descriptor tail, its first block's copy, its commit block, and
        // transaction 3's revoke tail, each followed by valid transactions: ending the log there
        // would lose them, so nothing is written and the volume still needs recovery.
        for (int damage = 0; damage < 4; ++damage)
        {
            INFO("damage " << damage);
            Journalled made = MakeJournalled(journal_case);
            RawVolume raw{made.original};
            const std::uint32_t size = made.block_size;
            const std::uint32_t second = made.first + 1;
            std::uint64_t byte = 0;
            if (damage == 0)
            {
                byte = JournalOffset(raw,
                                     FindLogBlock(made.original, second, JournalDescriptorBlock)) +
                       size - 5;
            }
            else if (damage == 1)
            {
                byte = JournalOffset(
                           raw, FindLogBlock(made.original, second, JournalDescriptorBlock) + 1) +
                       100;
            }
            else if (damage == 2)
            {
                byte = JournalOffset(raw, FindLogBlock(made.original, second, JournalCommitBlock)) +
                       100;
            }
            else
            {
                byte = JournalOffset(raw,
                                     FindLogBlock(made.original, second + 1, JournalRevokeBlock)) +
                       size - 5;
            }
            made.device->Bytes()[byte] ^= 0x20U;
            const std::vector<std::uint8_t> damaged = BytesOf(*made.device);
            REQUIRE(ErrorOf(Ext2Type().Mount(*made.device, {.read_only = false})) ==
                    Error::Corrupt);
            CheckUnchanged(*made.device, damaged);
            REQUIRE(made.device->flush_count == 0);
        }
    }
#endif
}

TEST_CASE("a jbd2 checksum that does not match in the torn tail only ends the log")
{
#ifndef FEATHERDISK_EXT_IMAGES
    SKIP("e2fsprogs is not installed");
#else
    for (const JournalCase& journal_case : {Cases[1], Cases[3], Cases[4], Cases[5]})
    {
        INFO(journal_case.image << " " << journal_case.open);
        // Transaction 5 never committed; its damaged descriptor changes nothing.
        Journalled made = MakeJournalled(journal_case);
        RawVolume raw{made.original};
        const std::uint32_t size = made.block_size;
        made.device->Bytes()[JournalOffset(raw, FindLogBlock(made.original, made.first + 4,
                                                             JournalDescriptorBlock)) +
                             size - 5] ^= 0x20U;
        Replay(*made.device);
        CheckClean(*made.device, "journal-torn-checksum");
        CheckEmptyJournal(*made.device, made.first + 5);
        REQUIRE(BlockText(*made.device, made.spare[1], size) == "written again after its revoke");

        // With async_commit a commit block may reach the disk before its transaction, so one
        // that fails its checksum ends the log: transaction 1 is replayed, 2 and later are not.
        made = MakeJournalled(journal_case);
        RawVolume again{made.original};
        const std::uint64_t journal = JournalOffset(again, 0);
        JournalSuperblock superblock = JournalSuperblockOf(*made.device);
        superblock.incompatible |= FeatherDisk::Ext::JournalIncompatibleAsyncCommit;
        StoreBigEndian32(made.device->Bytes().data() + journal + 40, superblock.incompatible);
        FeatherDisk::Ext::EncodeJournalPosition(superblock,
                                                made.device->Bytes().subspan(journal, size));
        made.device->Bytes()[JournalOffset(again, FindLogBlock(made.original, made.first + 1,
                                                               JournalCommitBlock)) +
                             100] ^= 0x20U;
        Replay(*made.device);
        CheckClean(*made.device, "journal-async-commit");
        CheckEmptyJournal(*made.device, made.first + 2);
        REQUIRE(BlockText(*made.device, made.hello_block, size) == NewHello);
        REQUIRE(SameBlock(*made.device, made.original, made.spare[0], size));
        REQUIRE(SameBlock(*made.device, made.original, made.spare[1], size));
    }
#endif
}

TEST_CASE("a malformed journal fails a writable mount before anything is written")
{
#ifndef FEATHERDISK_EXT_IMAGES
    SKIP("e2fsprogs is not installed");
#else
    const auto refused = [](Journalled& made, Error error)
    {
        made.original = BytesOf(*made.device);
        REQUIRE(ErrorOf(Ext2Type().Mount(*made.device, {.read_only = false})) == error);
        CheckUnchanged(*made.device, made.original);
        // A read-only mount does not look at the journal.
        MountedFilesystem ext(Ext2Type(), *made.device, true);
        REQUIRE(ReadAll(*ext, Value(Walk(*ext, "hello.txt"))) == OldHello);
    };
    const auto store_journal = [](Journalled& made, std::size_t offset, std::uint32_t value)
    {
        std::vector<std::uint8_t> bytes = BytesOf(*made.device);
        const RawVolume raw{bytes};
        StoreBigEndian32(made.device->Bytes().data() + JournalOffset(raw, 0) + offset, value);
    };
    // Without journal checksums, so only the damage itself is wrong.
    const JournalCase plain = Cases[2];

    SECTION("a committed transaction names a block past the volume")
    {
        Journalled made = MakeJournalled(plain);
        RawVolume raw{made.original};
        const std::uint64_t tag = JournalOffset(raw, FindLogBlock(made.original, made.first + 1,
                                                                  JournalDescriptorBlock)) +
                                  12;
        StoreBigEndian32(made.device->Bytes().data() + tag, raw.BlockCount() + 5);
        refused(made, Error::Corrupt);
    }
    SECTION("a revoke block's count runs past the block")
    {
        Journalled made = MakeJournalled(plain);
        RawVolume raw{made.original};
        const std::uint64_t count =
            JournalOffset(raw, FindLogBlock(made.original, made.first + 2, JournalRevokeBlock)) +
            12;
        StoreBigEndian32(made.device->Bytes().data() + count, made.block_size + 8);
        refused(made, Error::Corrupt);
    }
    SECTION("the log starts outside the ring")
    {
        Journalled made = MakeJournalled(plain);
        store_journal(made, 28, JournalSuperblockOf(*made.device).maximum_length);
        refused(made, Error::Corrupt);
    }
    SECTION("the journal claims more blocks than its inode holds")
    {
        Journalled made = MakeJournalled(plain);
        store_journal(made, 16, JournalSuperblockOf(*made.device).maximum_length + 1);
        refused(made, Error::Corrupt);
    }
    SECTION("the journal superblock has another block size or version")
    {
        Journalled made = MakeJournalled(plain);
        store_journal(made, 12, made.block_size * 2);
        refused(made, Error::Corrupt);
        made = MakeJournalled(plain);
        store_journal(made, 4, 7);
        refused(made, Error::Corrupt);
    }
    SECTION("the journal superblock's checksum does not match")
    {
        Journalled made = MakeJournalled(Cases[3]);
        std::vector<std::uint8_t> bytes = BytesOf(*made.device);
        const RawVolume raw{bytes};
        made.device->Bytes()[JournalOffset(raw, 0) + 200] ^= 1U;
        refused(made, Error::Corrupt);
    }
    SECTION("an uncommitted log that fills the ring goes round it")
    {
        // Every block a descriptor of transaction 1 with one tag, so each tagged block is the
        // next descriptor's and the walk never meets a commit block or another sequence.
        Journalled made = MakeJournalled(plain);
        RawVolume raw{made.original};
        const JournalSuperblock journal = JournalSuperblockOf(*made.device);
        for (std::uint32_t position = journal.first; position < journal.maximum_length; ++position)
        {
            std::uint8_t* block = made.device->Bytes().data() + JournalOffset(raw, position);
            std::fill(block, block + made.block_size, std::uint8_t{0});
            StoreBigEndian32(block, JournalMagic);
            StoreBigEndian32(block + 4, JournalDescriptorBlock);
            StoreBigEndian32(block + 8, 1);
            StoreBigEndian32(block + 12, static_cast<std::uint32_t>(made.spare[0]));
            block[12 + 7] = 2 | 8;
        }
        store_journal(made, 24, 1);
        store_journal(made, 28, journal.first);
        refused(made, Error::Corrupt);
    }
#endif
}

TEST_CASE("a journal recovery cannot do leaves the volume to a read-only mount")
{
#ifndef FEATHERDISK_EXT_IMAGES
    SKIP("e2fsprogs is not installed");
#else
    // Unknown journal features, and a journal on another device.
    for (int variant = 0; variant < 2; ++variant)
    {
        INFO("variant " << variant);
        Journalled made = MakeJournalled(Cases[2]);
        std::vector<std::uint8_t> bytes = BytesOf(*made.device);
        RawVolume raw{bytes};
        if (variant == 0)
        {
            StoreBigEndian32(made.device->Bytes().data() + JournalOffset(raw, 0) + 40,
                             JournalIncompatible64Bit | 0x100U);
        }
        else
        {
            raw.Store32(SuperblockStart + 224, 0);
            raw.UpdateSuperblockChecksum();
            std::copy(bytes.begin(), bytes.end(), made.device->Bytes().begin());
        }
        made.original = BytesOf(*made.device);
        {
            MountedFilesystem ext(Ext2Type(), *made.device, false);
            REQUIRE_FALSE(ext->GetCapabilities().writable);
            REQUIRE(ReadAll(*ext, Value(Walk(*ext, "hello.txt"))) == OldHello);
        }
        CheckUnchanged(*made.device, made.original);
    }
#endif
}

TEST_CASE("an empty journal that needs recovery is only marked recovered")
{
#ifndef FEATHERDISK_EXT_IMAGES
    SKIP("e2fsprogs is not installed");
#else
    for (const std::string_view image : {"ext3", "ext4-blockmap"})
    {
        INFO(image);
        std::vector<std::uint8_t> bytes = LoadExtImage(image);
        RawVolume raw{bytes};
        raw.Store32(SuperblockStart + 96, raw.Load32(SuperblockStart + 96) | NeedsRecovery);
        raw.UpdateSuperblockChecksum();
        const std::unique_ptr<MemoryDevice> device = DeviceFrom(bytes);
        const std::uint32_t sequence = JournalSuperblockOf(*device).sequence;
        const std::unique_ptr<MemoryDevice> expected = CopyOf(*device);
        REQUIRE(ChangeWithDebugfs(*expected, "journal-run-empty", "jr").status == 0);
        Replay(*device);
        RequireSameAsDebugfs(*device, *expected);
        CheckEmptyJournal(*device, sequence + 1);
        CheckClean(*device, "journal-empty");
    }
#endif
}

TEST_CASE("a replay stopped at any write replays again to the same volume")
{
#ifndef FEATHERDISK_EXT_IMAGES
    SKIP("e2fsprogs is not installed");
#else
    for (const JournalCase& journal_case : {Cases[0], Cases[3]})
    {
        INFO(journal_case.image << " " << journal_case.open);
        const Journalled made = MakeJournalled(journal_case);
        const std::unique_ptr<MemoryDevice> whole = CopyOf(*made.device);
        Replay(*whole);
        const std::uint64_t writes = whole->write_count;
        REQUIRE(writes > 4);
        // The journal superblock's sequence moves on with each recovery that finds the log empty.
        const auto without_sequence = [](MemoryDevice& device)
        {
            std::vector<std::uint8_t> bytes = BytesOf(device);
            const RawVolume raw{bytes};
            const std::uint64_t journal = JournalOffset(raw, 0);
            std::fill_n(bytes.begin() + static_cast<std::ptrdiff_t>(journal + 24), 4, 0);
            std::fill_n(bytes.begin() + static_cast<std::ptrdiff_t>(journal + 252), 4, 0);
            return bytes;
        };
        for (std::uint64_t allowed = 0; allowed < writes; ++allowed)
        {
            INFO("writes allowed " << allowed);
            const std::unique_ptr<MemoryDevice> stopped = CopyOf(*made.device);
            stopped->FailWritesAfter(allowed);
            REQUIRE(ErrorOf(Ext2Type().Mount(*stopped, {.read_only = false})) == Error::Io);
            const std::unique_ptr<MemoryDevice> again = CopyOf(*stopped);
            Replay(*again);
            RequireSameBytes(without_sequence(*again), without_sequence(*whole));
        }
    }
#endif
}

TEST_CASE("a log that wraps past the end of the journal replays in order")
{
#ifndef FEATHERDISK_EXT_IMAGES
    SKIP("e2fsprogs is not installed");
#else
    for (const JournalCase& journal_case : {Cases[2], Cases[3]})
    {
        INFO(journal_case.image << " " << journal_case.open);
        // The log moved to start five blocks before the ring's end; no block's checksum depends
        // on where it is.
        Journalled made = MakeJournalled(journal_case);
        RawVolume raw{made.original};
        JournalSuperblock journal = JournalSuperblockOf(*made.device);
        const std::uint32_t ring = journal.maximum_length - journal.first;
        const std::uint32_t start = journal.maximum_length - 5;
        REQUIRE(LogBlocks(made.original).size() > 5);
        std::vector<std::vector<std::uint8_t>> log;
        for (std::uint32_t position = journal.first; position < journal.maximum_length; ++position)
        {
            const std::uint64_t offset = JournalOffset(raw, position);
            log.emplace_back(made.original.begin() + static_cast<std::ptrdiff_t>(offset),
                             made.original.begin() +
                                 static_cast<std::ptrdiff_t>(offset + made.block_size));
        }
        for (std::uint32_t index = 0; index < ring; ++index)
        {
            const std::uint32_t position = journal.first + (start - journal.first + index) % ring;
            std::copy(log[index].begin(), log[index].end(),
                      made.device->Bytes().begin() +
                          static_cast<std::ptrdiff_t>(JournalOffset(raw, position)));
        }
        journal.start = start;
        FeatherDisk::Ext::EncodeJournalPosition(
            journal, made.device->Bytes().subspan(JournalOffset(raw, 0), made.block_size));
        const std::unique_ptr<MemoryDevice> expected = CopyOf(*made.device);
        REQUIRE(ChangeWithDebugfs(*expected, "journal-run-wrapped", "jr").status == 0);
        Replay(*made.device);
        RequireSameAsDebugfs(*made.device, *expected);
        CheckEmptyJournal(*made.device, made.first + 5);
        CheckClean(*made.device, "journal-wrapped");
        REQUIRE(BlockText(*made.device, made.hello_block, made.block_size) == NewHello);
        REQUIRE(BlockText(*made.device, made.spare[1], made.block_size) ==
                "written again after its revoke");
    }
#endif
}
