// SPDX-License-Identifier: MIT OR Apache-2.0
// jbd2, the journal ext3 and ext4 share, in which every field is big-endian. Block 0 of the
// journal is its superblock; the blocks from s_first to s_maxlen form a ring, the log, which from
// s_start holds transactions in sequence order: descriptor blocks, each followed by the blocks its
// tags name, revoke blocks, and a commit block. Every block of a transaction carries its sequence.
#ifndef FEATHERDISK_EXT_JOURNAL_H
#define FEATHERDISK_EXT_JOURNAL_H

#include "fs/error.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace FeatherDisk::Ext
{

constexpr std::uint32_t JournalMagic = 0xC03B3998;
constexpr std::size_t JournalHeaderSize = 12;
constexpr std::size_t JournalSuperblockSize = 1024;
constexpr std::size_t JournalUuidSize = 16;
// The checksum at the end of descriptor and revoke blocks with checksums v2 or v3.
constexpr std::size_t JournalTailSize = 4;

// h_blocktype
constexpr std::uint32_t JournalDescriptorBlock = 1;
constexpr std::uint32_t JournalCommitBlock = 2;
constexpr std::uint32_t JournalSuperblockVersion1 = 3;
constexpr std::uint32_t JournalSuperblockVersion2 = 4;
constexpr std::uint32_t JournalRevokeBlock = 5;

// s_feature_incompat; version 1 superblocks have no features.
constexpr std::uint32_t JournalIncompatibleRevoke = 0x01;
constexpr std::uint32_t JournalIncompatible64Bit = 0x02;
// Commit blocks written without waiting for the transaction's other blocks.
constexpr std::uint32_t JournalIncompatibleAsyncCommit = 0x04;
constexpr std::uint32_t JournalIncompatibleChecksumV2 = 0x08;
constexpr std::uint32_t JournalIncompatibleChecksumV3 = 0x10;
constexpr std::uint32_t JournalIncompatibleKnown =
    JournalIncompatibleRevoke | JournalIncompatible64Bit | JournalIncompatibleAsyncCommit |
    JournalIncompatibleChecksumV2 | JournalIncompatibleChecksumV3;
// s_checksum_type with checksums v2 or v3.
constexpr std::uint8_t JournalChecksumCrc32c = 4;

// t_flags
// The block began with JournalMagic, which the journal's copy has as zeros.
constexpr std::uint16_t JournalTagEscaped = 0x1;
// No UUID follows the tag: it is the previous tag's.
constexpr std::uint16_t JournalTagSameUuid = 0x2;
constexpr std::uint16_t JournalTagLast = 0x8;

std::uint32_t LoadBigEndian32(const std::uint8_t* bytes);
void StoreBigEndian32(std::uint8_t* bytes, std::uint32_t value);

struct JournalHeader
{
    std::uint32_t magic;
    std::uint32_t block_type;
    std::uint32_t sequence;
};

JournalHeader DecodeJournalHeader(std::span<const std::uint8_t> block);

struct JournalSuperblock
{
    JournalHeader header;
    std::uint32_t block_size;
    // s_maxlen: the journal's blocks, the superblock's included.
    std::uint32_t maximum_length;
    std::uint32_t first;
    // The first transaction the log holds, which starts at block start; 0 when the log is empty.
    std::uint32_t sequence;
    std::uint32_t start;
    std::uint32_t incompatible;
    std::uint8_t checksum_type;
    std::array<std::uint8_t, JournalUuidSize> uuid;
    // Without checksums always true.
    bool checksum_matches;
};

// block holds at least JournalSuperblockSize bytes.
JournalSuperblock DecodeJournalSuperblock(std::span<const std::uint8_t> block);
// Error::Unsupported for unknown incompatible features or asynchronous commits without checksums
// v2 or v3; Error::Corrupt for anything else that does not hold together: the magic, the version,
// both checksum versions, an unknown checksum type, a checksum that does not match, another block
// size, or a log that does not fit in journal_blocks.
Status CheckJournalSuperblock(const JournalSuperblock& superblock, std::uint32_t block_size,
                              std::uint64_t journal_blocks);
// Stores sequence and start, and the checksum again when the journal has checksums.
void EncodeJournalPosition(const JournalSuperblock& superblock, std::span<std::uint8_t> block);

bool JournalHasChecksums(std::uint32_t incompatible);
// What every block checksum starts from: the CRC32C of the journal's UUID.
std::uint32_t JournalChecksumSeed(const JournalSuperblock& superblock);
// The bytes of one descriptor tag (without the UUID that may follow it): 16 with checksums v3,
// otherwise 8, 4 more with 64-bit block numbers and 2 more with checksums v2.
std::size_t JournalTagSize(std::uint32_t incompatible);

struct JournalTag
{
    std::uint64_t block;
    std::uint16_t flags;
    // 16 bits with checksums v2, 32 with v3.
    std::uint32_t checksum;
};

// The tags of a descriptor block, in order.
class JournalTags
{
public:
    JournalTags(std::span<const std::uint8_t> block, std::uint32_t incompatible);

    std::optional<JournalTag> Next();

private:
    std::span<const std::uint8_t> block;
    std::uint32_t incompatible;
    std::size_t offset;
    std::size_t end;
    bool done;
};

// A descriptor or revoke block's tail.
bool JournalTailMatches(std::span<const std::uint8_t> block, std::uint32_t seed);
bool JournalCommitMatches(std::span<const std::uint8_t> block, std::uint32_t seed);
// data is the block as the journal holds it, escaped.
bool JournalTagMatches(const JournalTag& tag, std::uint32_t incompatible, std::uint32_t seed,
                       std::uint32_t sequence, std::span<const std::uint8_t> data);

// Sequence numbers wrap, so they compare by their difference, as jbd2's tid_gt does.
bool SequenceBefore(std::uint32_t first, std::uint32_t second);

// For each block a revoke record names, the latest transaction that revoked it: copies of the
// block in that transaction or earlier ones must not be replayed.
class RevokeTable
{
public:
    RevokeTable() = default;
    ~RevokeTable();
    RevokeTable(const RevokeTable&) = delete;
    RevokeTable& operator=(const RevokeTable&) = delete;

    Status Add(std::uint64_t block, std::uint32_t sequence);
    bool Revokes(std::uint64_t block, std::uint32_t sequence) const;
    std::size_t Count() const;

private:
    struct Entry
    {
        std::uint64_t block;
        std::uint32_t sequence;
        bool used;
    };

    Entry* Find(std::uint64_t block) const;
    Status Grow();

    Entry* entries = nullptr;
    std::size_t capacity = 0;
    std::size_t count = 0;
};

// Whether r_count keeps the records inside the block (and before its tail).
bool RevokeRecordsFit(std::span<const std::uint8_t> block, std::uint32_t incompatible);
// Adds the records of a revoke block that RevokeRecordsFit accepted.
Status ReadRevokeRecords(std::span<const std::uint8_t> block, std::uint32_t incompatible,
                         std::uint32_t sequence, RevokeTable& table);

} // namespace FeatherDisk::Ext

#endif
