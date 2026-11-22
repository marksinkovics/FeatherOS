// SPDX-License-Identifier: MIT OR Apache-2.0
#include "ext/journal.h"

#include "common/crc.h"
#include "fs/host.h"

namespace FeatherDisk::Ext
{
namespace
{

constexpr std::size_t SuperblockBlockSizeOffset = 12;
constexpr std::size_t SuperblockMaximumLengthOffset = 16;
constexpr std::size_t SuperblockFirstOffset = 20;
constexpr std::size_t SuperblockSequenceOffset = 24;
constexpr std::size_t SuperblockStartOffset = 28;
constexpr std::size_t SuperblockCompatibleOffset = 36;
constexpr std::size_t SuperblockIncompatibleOffset = 40;
constexpr std::size_t SuperblockUuidOffset = 48;
constexpr std::size_t SuperblockChecksumTypeOffset = 80;
constexpr std::size_t SuperblockChecksumOffset = 252;
// h_chksum[0] of a commit block, after h_chksum_type, h_chksum_size and two bytes of padding.
constexpr std::size_t CommitChecksumOffset = 16;
constexpr std::size_t CommitSecondsOffset = 48;
constexpr std::size_t CommitNanosecondsOffset = 56;
constexpr std::size_t RevokeCountOffset = 12;
constexpr std::size_t RevokeHeaderSize = 16;
constexpr std::size_t FirstRevokeCapacity = 64;

std::uint16_t LoadBigEndian16(const std::uint8_t* bytes)
{
    return static_cast<std::uint16_t>((std::uint32_t{bytes[0]} << 8) | bytes[1]);
}

// The CRC32C of bytes with the four at offset taken as zeros, which is how jbd2 checksums a
// block that holds its own checksum.
std::uint32_t ChecksumWithout(std::uint32_t seed, std::span<const std::uint8_t> bytes,
                              std::size_t offset)
{
    constexpr std::array<std::uint8_t, 4> Zeros{};
    std::uint32_t crc = Crc32c(seed, bytes.first(offset));
    crc = Crc32c(crc, Zeros);
    return Crc32c(crc, bytes.subspan(offset + Zeros.size()));
}

void StoreBigEndian16(std::uint8_t* bytes, std::uint16_t value)
{
    bytes[0] = static_cast<std::uint8_t>(value >> 8);
    bytes[1] = static_cast<std::uint8_t>(value);
}

void StoreBigEndian64(std::uint8_t* bytes, std::uint64_t value)
{
    StoreBigEndian32(bytes, static_cast<std::uint32_t>(value >> 32));
    StoreBigEndian32(bytes + 4, static_cast<std::uint32_t>(value));
}

void ZeroBlock(std::span<std::uint8_t> block)
{
    for (std::uint8_t& byte : block)
    {
        byte = 0;
    }
}

std::size_t HashOf(std::uint64_t block, std::size_t capacity)
{
    constexpr std::uint64_t Golden = 0x9E3779B97F4A7C15;
    return static_cast<std::size_t>((block * Golden) >> 32) & (capacity - 1);
}

} // namespace

std::uint32_t LoadBigEndian32(const std::uint8_t* bytes)
{
    return (std::uint32_t{bytes[0]} << 24) | (std::uint32_t{bytes[1]} << 16) |
           (std::uint32_t{bytes[2]} << 8) | bytes[3];
}

void StoreBigEndian32(std::uint8_t* bytes, std::uint32_t value)
{
    bytes[0] = static_cast<std::uint8_t>(value >> 24);
    bytes[1] = static_cast<std::uint8_t>(value >> 16);
    bytes[2] = static_cast<std::uint8_t>(value >> 8);
    bytes[3] = static_cast<std::uint8_t>(value);
}

JournalHeader DecodeJournalHeader(std::span<const std::uint8_t> block)
{
    return {
        .magic = LoadBigEndian32(block.data()),
        .block_type = LoadBigEndian32(block.data() + 4),
        .sequence = LoadBigEndian32(block.data() + 8),
    };
}

JournalSuperblock DecodeJournalSuperblock(std::span<const std::uint8_t> block)
{
    const std::uint8_t* raw = block.data();
    JournalSuperblock superblock = {
        .header = DecodeJournalHeader(block),
        .block_size = LoadBigEndian32(raw + SuperblockBlockSizeOffset),
        .maximum_length = LoadBigEndian32(raw + SuperblockMaximumLengthOffset),
        .first = LoadBigEndian32(raw + SuperblockFirstOffset),
        .sequence = LoadBigEndian32(raw + SuperblockSequenceOffset),
        .start = LoadBigEndian32(raw + SuperblockStartOffset),
        .compatible = 0,
        .incompatible = 0,
        .checksum_type = 0,
        .uuid = {},
        .checksum_matches = true,
    };
    // Version 1 ends before the features.
    if (superblock.header.block_type != JournalSuperblockVersion2)
    {
        return superblock;
    }
    superblock.compatible = LoadBigEndian32(raw + SuperblockCompatibleOffset);
    superblock.incompatible = LoadBigEndian32(raw + SuperblockIncompatibleOffset);
    superblock.checksum_type = raw[SuperblockChecksumTypeOffset];
    for (std::size_t index = 0; index < JournalUuidSize; ++index)
    {
        superblock.uuid[index] = raw[SuperblockUuidOffset + index];
    }
    if (JournalHasChecksums(superblock.incompatible))
    {
        superblock.checksum_matches =
            ChecksumWithout(~0U, block.first(JournalSuperblockSize), SuperblockChecksumOffset) ==
            LoadBigEndian32(raw + SuperblockChecksumOffset);
    }
    return superblock;
}

Status CheckJournalSuperblock(const JournalSuperblock& superblock, std::uint32_t block_size,
                              std::uint64_t journal_blocks)
{
    const std::uint32_t version = superblock.header.block_type;
    if (superblock.header.magic != JournalMagic ||
        (version != JournalSuperblockVersion1 && version != JournalSuperblockVersion2))
    {
        return std::unexpected(Error::Corrupt);
    }
    if ((superblock.incompatible & ~JournalIncompatibleKnown) != 0)
    {
        return std::unexpected(Error::Unsupported);
    }
    // Commit blocks written before the rest of their transaction are only safe to trust with
    // checksums over every block, and the compatible v1 ones are not verified here.
    if ((superblock.incompatible & JournalIncompatibleAsyncCommit) != 0 &&
        !JournalHasChecksums(superblock.incompatible))
    {
        return std::unexpected(Error::Unsupported);
    }
    if (JournalHasChecksums(superblock.incompatible))
    {
        if ((superblock.incompatible & JournalIncompatibleChecksumV2) != 0 &&
            (superblock.incompatible & JournalIncompatibleChecksumV3) != 0)
        {
            return std::unexpected(Error::Corrupt);
        }
        if (superblock.checksum_type != JournalChecksumCrc32c || !superblock.checksum_matches)
        {
            return std::unexpected(Error::Corrupt);
        }
    }
    if (superblock.block_size != block_size || superblock.maximum_length > journal_blocks ||
        superblock.first == 0 || superblock.first >= superblock.maximum_length)
    {
        return std::unexpected(Error::Corrupt);
    }
    if (superblock.start != 0 &&
        (superblock.start < superblock.first || superblock.start >= superblock.maximum_length))
    {
        return std::unexpected(Error::Corrupt);
    }
    return {};
}

void EncodeJournalPosition(const JournalSuperblock& superblock, std::span<std::uint8_t> block)
{
    StoreBigEndian32(block.data() + SuperblockSequenceOffset, superblock.sequence);
    StoreBigEndian32(block.data() + SuperblockStartOffset, superblock.start);
    if (JournalHasChecksums(superblock.incompatible))
    {
        StoreBigEndian32(
            block.data() + SuperblockChecksumOffset,
            ChecksumWithout(~0U, block.first(JournalSuperblockSize), SuperblockChecksumOffset));
    }
}

void EncodeJournalFeatures(const JournalSuperblock& superblock, std::span<std::uint8_t> block)
{
    StoreBigEndian32(block.data() + SuperblockCompatibleOffset, superblock.compatible);
    StoreBigEndian32(block.data() + SuperblockIncompatibleOffset, superblock.incompatible);
    if (JournalHasChecksums(superblock.incompatible))
    {
        block[SuperblockChecksumTypeOffset] = JournalChecksumCrc32c;
        StoreBigEndian32(
            block.data() + SuperblockChecksumOffset,
            ChecksumWithout(~0U, block.first(JournalSuperblockSize), SuperblockChecksumOffset));
    }
}

bool JournalHasChecksums(std::uint32_t incompatible)
{
    return (incompatible & (JournalIncompatibleChecksumV2 | JournalIncompatibleChecksumV3)) != 0;
}

std::uint32_t JournalChecksumSeed(const JournalSuperblock& superblock)
{
    return Crc32c(~0U, superblock.uuid);
}

std::size_t JournalTagSize(std::uint32_t incompatible)
{
    if ((incompatible & JournalIncompatibleChecksumV3) != 0)
    {
        return 16;
    }
    std::size_t size = 8;
    if ((incompatible & JournalIncompatible64Bit) != 0)
    {
        size += 4;
    }
    if ((incompatible & JournalIncompatibleChecksumV2) != 0)
    {
        size += 2;
    }
    return size;
}

JournalTags::JournalTags(std::span<const std::uint8_t> block, std::uint32_t incompatible)
    : block(block), incompatible(incompatible), offset(JournalHeaderSize),
      end(block.size() - (JournalHasChecksums(incompatible) ? JournalTailSize : 0)), done(false)
{
}

// Both tag layouts keep t_blocknr first, the flags' low half at byte 6 and t_blocknr_high at
// byte 8; the checksum is the 16 bits at byte 4, or with v3 the 32 bits at byte 12.
std::optional<JournalTag> JournalTags::Next()
{
    const std::size_t size = JournalTagSize(incompatible);
    if (done || offset + size > end)
    {
        return std::nullopt;
    }
    const std::uint8_t* raw = block.data() + offset;
    JournalTag tag = {
        .block = LoadBigEndian32(raw),
        .flags = LoadBigEndian16(raw + 6),
        .checksum = (incompatible & JournalIncompatibleChecksumV3) != 0 ? LoadBigEndian32(raw + 12)
                                                                        : LoadBigEndian16(raw + 4),
    };
    if ((incompatible & JournalIncompatible64Bit) != 0)
    {
        tag.block |= std::uint64_t{LoadBigEndian32(raw + 8)} << 32;
    }
    offset += size;
    if ((tag.flags & JournalTagSameUuid) == 0)
    {
        offset += JournalUuidSize;
    }
    done = (tag.flags & JournalTagLast) != 0;
    return tag;
}

bool JournalTailMatches(std::span<const std::uint8_t> block, std::uint32_t seed)
{
    const std::size_t offset = block.size() - JournalTailSize;
    return ChecksumWithout(seed, block, offset) == LoadBigEndian32(block.data() + offset);
}

bool JournalCommitMatches(std::span<const std::uint8_t> block, std::uint32_t seed)
{
    return ChecksumWithout(seed, block, CommitChecksumOffset) ==
           LoadBigEndian32(block.data() + CommitChecksumOffset);
}

bool JournalTagMatches(const JournalTag& tag, std::uint32_t incompatible, std::uint32_t seed,
                       std::uint32_t sequence, std::span<const std::uint8_t> data)
{
    std::array<std::uint8_t, 4> raw_sequence{};
    StoreBigEndian32(raw_sequence.data(), sequence);
    const std::uint32_t crc = Crc32c(Crc32c(seed, raw_sequence), data);
    if ((incompatible & JournalIncompatibleChecksumV3) != 0)
    {
        return crc == tag.checksum;
    }
    return (crc & 0xFFFFU) == tag.checksum;
}

void StoreJournalHeader(std::span<std::uint8_t> block, std::uint32_t block_type,
                        std::uint32_t sequence)
{
    StoreBigEndian32(block.data(), JournalMagic);
    StoreBigEndian32(block.data() + 4, block_type);
    StoreBigEndian32(block.data() + 8, sequence);
}

bool EscapeJournalBlock(std::span<std::uint8_t> copy)
{
    if (LoadBigEndian32(copy.data()) != JournalMagic)
    {
        return false;
    }
    for (std::size_t index = 0; index < 4; ++index)
    {
        copy[index] = 0;
    }
    return true;
}

std::uint32_t JournalTagChecksum(std::uint32_t seed, std::uint32_t sequence,
                                 std::span<const std::uint8_t> copy)
{
    std::array<std::uint8_t, 4> raw_sequence{};
    StoreBigEndian32(raw_sequence.data(), sequence);
    return Crc32c(Crc32c(seed, raw_sequence), copy);
}

std::size_t JournalTagsPerDescriptor(std::uint32_t block_size, std::uint32_t incompatible)
{
    const std::size_t tail = JournalHasChecksums(incompatible) ? JournalTailSize : 0;
    const std::size_t tag = JournalTagSize(incompatible);
    // After n tags, n * tag + the UUID are used; another goes in while a tag, a UUID and the tail
    // still fit in what is left, which first fails for n = (size - header - UUID - tag - UUID -
    // tail) / tag.
    const std::size_t reserve = JournalHeaderSize + 2 * JournalUuidSize + tail;
    const std::size_t count = block_size > reserve ? (block_size - reserve) / tag : 0;
    return count == 0 ? 1 : count;
}

std::size_t JournalRevokesPerBlock(std::uint32_t block_size, std::uint32_t incompatible)
{
    const std::size_t tail = JournalHasChecksums(incompatible) ? JournalTailSize : 0;
    const std::size_t record = (incompatible & JournalIncompatible64Bit) != 0 ? 8 : 4;
    return (block_size - RevokeHeaderSize - tail) / record;
}

JournalDescriptorWriter::JournalDescriptorWriter(
    std::span<std::uint8_t> block, std::uint32_t incompatible, std::uint32_t sequence,
    std::span<const std::uint8_t, JournalUuidSize> uuid)
    : block(block), incompatible(incompatible), uuid{},
      capacity(JournalTagsPerDescriptor(static_cast<std::uint32_t>(block.size()), incompatible)),
      count(0), offset(JournalHeaderSize), last(0)
{
    for (std::size_t index = 0; index < JournalUuidSize; ++index)
    {
        this->uuid[index] = uuid[index];
    }
    ZeroBlock(block);
    StoreJournalHeader(block, JournalDescriptorBlock, sequence);
}

bool JournalDescriptorWriter::Add(std::uint64_t home, bool escaped, std::uint32_t checksum)
{
    if (count == capacity)
    {
        return false;
    }
    const bool version3 = (incompatible & JournalIncompatibleChecksumV3) != 0;
    std::uint8_t* raw = block.data() + offset;
    std::uint16_t flags = escaped ? JournalTagEscaped : 0;
    if (count != 0)
    {
        flags |= JournalTagSameUuid;
    }
    StoreBigEndian32(raw, static_cast<std::uint32_t>(home));
    if ((incompatible & JournalIncompatible64Bit) != 0)
    {
        StoreBigEndian32(raw + 8, static_cast<std::uint32_t>(home >> 32));
    }
    if (version3)
    {
        StoreBigEndian32(raw + 4, flags);
        StoreBigEndian32(raw + 12, checksum);
    }
    else
    {
        if ((incompatible & JournalIncompatibleChecksumV2) != 0)
        {
            StoreBigEndian16(raw + 4, static_cast<std::uint16_t>(checksum));
        }
        StoreBigEndian16(raw + 6, flags);
    }
    last = offset;
    offset += JournalTagSize(incompatible);
    if (count == 0)
    {
        for (std::size_t index = 0; index < JournalUuidSize; ++index)
        {
            block[offset + index] = uuid[index];
        }
        offset += JournalUuidSize;
    }
    ++count;
    return true;
}

std::size_t JournalDescriptorWriter::Count() const
{
    return count;
}

void JournalDescriptorWriter::Finish(std::uint32_t seed)
{
    if (count != 0)
    {
        // The low byte of the flags, which both layouts keep at byte 7 of the tag.
        block[last + 7] = static_cast<std::uint8_t>(block[last + 7] | JournalTagLast);
    }
    if (JournalHasChecksums(incompatible))
    {
        const std::size_t tail = block.size() - JournalTailSize;
        StoreBigEndian32(block.data() + tail, ChecksumWithout(seed, block, tail));
    }
}

JournalRevokeWriter::JournalRevokeWriter(std::span<std::uint8_t> block, std::uint32_t incompatible,
                                         std::uint32_t sequence)
    : bytes(block), incompatible(incompatible),
      capacity(JournalRevokesPerBlock(static_cast<std::uint32_t>(block.size()), incompatible)),
      count(0)
{
    ZeroBlock(block);
    StoreJournalHeader(block, JournalRevokeBlock, sequence);
    StoreBigEndian32(block.data() + RevokeCountOffset, RevokeHeaderSize);
}

bool JournalRevokeWriter::Add(std::uint64_t block)
{
    if (count == capacity)
    {
        return false;
    }
    const bool wide = (incompatible & JournalIncompatible64Bit) != 0;
    const std::size_t record = wide ? 8 : 4;
    std::uint8_t* raw = bytes.data() + RevokeHeaderSize + count * record;
    if (wide)
    {
        StoreBigEndian64(raw, block);
    }
    else
    {
        StoreBigEndian32(raw, static_cast<std::uint32_t>(block));
    }
    ++count;
    StoreBigEndian32(bytes.data() + RevokeCountOffset,
                     static_cast<std::uint32_t>(RevokeHeaderSize + count * record));
    return true;
}

std::size_t JournalRevokeWriter::Count() const
{
    return count;
}

void JournalRevokeWriter::Finish(std::uint32_t seed)
{
    if (JournalHasChecksums(incompatible))
    {
        const std::size_t tail = bytes.size() - JournalTailSize;
        StoreBigEndian32(bytes.data() + tail, ChecksumWithout(seed, bytes, tail));
    }
}

void EncodeCommitBlock(std::span<std::uint8_t> block, std::uint32_t incompatible,
                       std::uint32_t seed, std::uint32_t sequence, std::int64_t seconds,
                       std::uint32_t nanoseconds)
{
    ZeroBlock(block);
    StoreJournalHeader(block, JournalCommitBlock, sequence);
    StoreBigEndian64(block.data() + CommitSecondsOffset, static_cast<std::uint64_t>(seconds));
    StoreBigEndian32(block.data() + CommitNanosecondsOffset, nanoseconds);
    if (JournalHasChecksums(incompatible))
    {
        StoreBigEndian32(block.data() + CommitChecksumOffset,
                         ChecksumWithout(seed, block, CommitChecksumOffset));
    }
}

bool SequenceBefore(std::uint32_t first, std::uint32_t second)
{
    return static_cast<std::int32_t>(first - second) < 0;
}

RevokeTable::~RevokeTable()
{
    Host::Free(entries);
}

RevokeTable::Entry* RevokeTable::Find(std::uint64_t block) const
{
    for (std::size_t index = HashOf(block, capacity);; index = (index + 1) & (capacity - 1))
    {
        Entry& entry = entries[index];
        if (!entry.used || entry.block == block)
        {
            return &entry;
        }
    }
}

Status RevokeTable::Grow()
{
    const std::size_t grown = capacity == 0 ? FirstRevokeCapacity : capacity * 2;
    void* memory = Host::Allocate(grown * sizeof(Entry));
    if (memory == nullptr)
    {
        return std::unexpected(Error::OutOfMemory);
    }
    Entry* old_entries = entries;
    const std::size_t old_capacity = capacity;
    entries = static_cast<Entry*>(memory);
    capacity = grown;
    for (std::size_t index = 0; index < capacity; ++index)
    {
        entries[index] = Entry{.block = 0, .sequence = 0, .used = false};
    }
    for (std::size_t index = 0; index < old_capacity; ++index)
    {
        if (old_entries[index].used)
        {
            *Find(old_entries[index].block) = old_entries[index];
        }
    }
    Host::Free(old_entries);
    return {};
}

Status RevokeTable::Add(std::uint64_t block, std::uint32_t sequence)
{
    // At most half full, so probes stay short and always end at an unused entry.
    if ((count + 1) * 2 > capacity)
    {
        if (const Status grown = Grow(); !grown)
        {
            return grown;
        }
    }
    Entry* entry = Find(block);
    if (!entry->used)
    {
        *entry = Entry{.block = block, .sequence = sequence, .used = true};
        ++count;
    }
    else if (SequenceBefore(entry->sequence, sequence))
    {
        entry->sequence = sequence;
    }
    return {};
}

bool RevokeTable::Revokes(std::uint64_t block, std::uint32_t sequence) const
{
    if (capacity == 0)
    {
        return false;
    }
    const Entry* entry = Find(block);
    return entry->used && !SequenceBefore(entry->sequence, sequence);
}

std::size_t RevokeTable::Count() const
{
    return count;
}

bool RevokeRecordsFit(std::span<const std::uint8_t> block, std::uint32_t incompatible)
{
    const std::size_t room =
        block.size() - (JournalHasChecksums(incompatible) ? JournalTailSize : 0);
    return LoadBigEndian32(block.data() + RevokeCountOffset) <= room;
}

Status ReadRevokeRecords(std::span<const std::uint8_t> block, std::uint32_t incompatible,
                         std::uint32_t sequence, RevokeTable& table)
{
    const std::size_t used = LoadBigEndian32(block.data() + RevokeCountOffset);
    const std::size_t record = (incompatible & JournalIncompatible64Bit) != 0 ? 8 : 4;
    for (std::size_t offset = RevokeHeaderSize; offset + record <= used; offset += record)
    {
        std::uint64_t revoked = LoadBigEndian32(block.data() + offset);
        if (record == 8)
        {
            revoked = (revoked << 32) | LoadBigEndian32(block.data() + offset + 4);
        }
        if (const Status added = table.Add(revoked, sequence); !added)
        {
            return added;
        }
    }
    return {};
}

} // namespace FeatherDisk::Ext
