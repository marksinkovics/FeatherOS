// SPDX-License-Identifier: MIT OR Apache-2.0
// Journal recovery, in jbd2's three passes over the log: the scan finds the first transaction
// without a valid commit block, the second collects the revoke records of the transactions before
// it, and the third copies their blocks to their places in log order, skipping a block revoked
// in its own transaction or a later one. Only a torn tail ends the log: a transaction whose commit
// block never comes, or (with async_commit) fails its checksum. Everything is checked during the
// scan, before anything is written, and a committed transaction whose descriptor, revoke, commit or
// block checksum fails, that names a block outside the volume or whose revoke count overruns its
// block, as well as a journal superblock that does not hold together, a hole in the journal or a
// log that runs round the whole ring, make the volume Error::Corrupt with needs_recovery still set:
// emptying the journal past them would lose the transactions after them.
//
// Afterwards the journal is left as e2fsck leaves it: s_start 0 (empty) and s_sequence one past
// the first transaction not replayed, so no stale commit block can be taken for a new one. Then
// needs_recovery goes from the superblock, which the replay may have rewritten. A crash at any
// point replays the same transactions again, or finds an empty journal.
#include "common/little_endian.h"
#include "ext/ext2_filesystem.h"
#include "ext/journal.h"
#include "ext/transaction.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace FeatherDisk::Ext
{

void Ext2Filesystem::JournalLog::Restart()
{
    position = header.start;
    remaining = std::uint64_t{header.maximum_length} - header.first + 1;
}

Result<std::uint32_t> Ext2Filesystem::JournalLog::Advance()
{
    if (remaining == 0)
    {
        LogWarning("ext2: ", "the journal's log runs round the whole ring");
        return std::unexpected(Error::Corrupt);
    }
    --remaining;
    const std::uint32_t current = position;
    position = current + 1 == header.maximum_length ? header.first : current + 1;
    return current;
}

Status Ext2Filesystem::RecoverJournal()
{
    Buffers buffers(block_size, 3);
    if (buffers.Empty())
    {
        return std::unexpected(Error::OutOfMemory);
    }
    JournalLog log{};
    if (const Status opened = OpenJournal(log, buffers); !opened)
    {
        return opened;
    }
    const std::uint32_t first = log.header.sequence;
    log.end = first;
    if (log.header.start != 0)
    {
        const Result<std::uint32_t> end = WalkLog(log, LogPass::Scan, buffers);
        if (!end)
        {
            return Failure(end);
        }
        log.end = *end;
        const Result<std::uint32_t> revoked = WalkLog(log, LogPass::Revoke, buffers);
        if (!revoked)
        {
            return Failure(revoked);
        }
        const Result<std::uint32_t> replayed = WalkLog(log, LogPass::Replay, buffers);
        if (!replayed)
        {
            return Failure(replayed);
        }
        // Only a block replayed into the journal itself could change the log between passes.
        if (*revoked != log.end || *replayed != log.end)
        {
            LogWarning("ext2: ", "the journal changed while it was replayed");
            return std::unexpected(Error::Corrupt);
        }
        if (const Status flushed = volume.Flush(); !flushed)
        {
            return flushed;
        }
    }
    log.header.sequence = log.end + 1;
    log.header.start = 0;
    const Result<std::uint64_t> place = LogBlockPlace(log, 0, buffers[2]);
    if (!place)
    {
        return Failure(place);
    }
    if (const Status read = cache.Read(*place, 0, buffers[0], buffers[0]); !read)
    {
        return read;
    }
    EncodeJournalPosition(log.header, buffers[0]);
    if (const Status written = cache.Write(*place, buffers[0]); !written)
    {
        return written;
    }
    if (const Status flushed = volume.Flush(); !flushed)
    {
        return flushed;
    }
    if (const Status cleared = ClearRecoveryFlag(buffers[0]); !cleared)
    {
        return cleared;
    }
    if (const Status flushed = volume.Flush(); !flushed)
    {
        return flushed;
    }
    LogWithValue("ext2: journal recovered; transactions replayed: ", log.end - first);
    return {};
}

Status Ext2Filesystem::OpenJournal(JournalLog& log, Buffers& buffers, bool report)
{
    if ((superblock.compatible & CompatibleHasJournal) == 0)
    {
        if (report)
        {
            LogWarning("ext2: cannot recover: ", "needs_recovery without a journal");
        }
        return std::unexpected(Error::Unsupported);
    }
    if (superblock.journal_inode == 0)
    {
        if (report)
        {
            LogWarning("ext2: cannot recover: ", "the journal is on another device");
        }
        return std::unexpected(Error::Unsupported);
    }
    const Result<Node> node = LoadNode(superblock.journal_inode, buffers[2]);
    if (!node || node->type != NodeType::File)
    {
        const Error error = node ? Error::Corrupt : node.error();
        if (error == Error::Io || error == Error::OutOfMemory)
        {
            return std::unexpected(error);
        }
        if (report)
        {
            LogWithValue("ext2: the journal inode is not a file: ", superblock.journal_inode);
        }
        return std::unexpected(Error::Corrupt);
    }
    log.inode = node->inode;
    if (const Status read = ReadLogBlock(log, 0, buffers[0], buffers[2]); !read)
    {
        return read;
    }
    log.header = DecodeJournalSuperblock(buffers[0]);
    if (const Status checked =
            CheckJournalSuperblock(log.header, block_size, log.inode.size / block_size);
        !checked)
    {
        if (!report)
        {
            return checked;
        }
        if (checked.error() == Error::Unsupported)
        {
            LogWithValue("ext2: cannot recover: unknown journal features ",
                         log.header.incompatible & ~JournalIncompatibleKnown);
        }
        else
        {
            LogWarning("ext2: ", "the journal superblock does not hold together");
        }
        return checked;
    }
    log.seed = JournalChecksumSeed(log.header);
    return {};
}

Result<std::uint64_t> Ext2Filesystem::LogBlockPlace(JournalLog& log, std::uint32_t position,
                                                    std::span<std::uint8_t> scratch)
{
    if (position - log.run.logical >= log.run.count)
    {
        const Result<BlockRun> mapped = MapRun(log.inode, position, scratch);
        if (!mapped)
        {
            return Failure(mapped);
        }
        log.run = *mapped;
    }
    if (log.run.physical == 0)
    {
        LogWithValue("ext2: the journal has a hole at its block ", position);
        return std::unexpected(Error::Corrupt);
    }
    return log.run.physical + (position - log.run.logical);
}

Status Ext2Filesystem::ReadLogBlock(JournalLog& log, std::uint32_t position,
                                    std::span<std::uint8_t> out, std::span<std::uint8_t> scratch)
{
    const Result<std::uint64_t> place = LogBlockPlace(log, position, scratch);
    if (!place)
    {
        return Failure(place);
    }
    return cache.ReadUncached(*place, out);
}

Result<std::uint32_t> Ext2Filesystem::WalkLog(JournalLog& log, LogPass pass, Buffers& buffers)
{
    const std::span<std::uint8_t> block = buffers[0];
    const bool checksums = JournalHasChecksums(log.header.incompatible);
    std::uint32_t sequence = log.header.sequence;
    TransactionCheck check{};
    log.Restart();
    while (pass == LogPass::Scan || SequenceBefore(sequence, log.end))
    {
        const Result<std::uint32_t> position = log.Advance();
        if (!position)
        {
            return Failure(position);
        }
        if (const Status read = ReadLogBlock(log, *position, block, buffers[2]); !read)
        {
            return Failure(read);
        }
        const JournalHeader header = DecodeJournalHeader(block);
        if (header.magic != JournalMagic || header.sequence != sequence)
        {
            break;
        }
        if (header.block_type == JournalCommitBlock)
        {
            if (pass == LogPass::Scan)
            {
                if (const Result<bool> ends = CheckCommit(log, sequence, check, block); !ends)
                {
                    return Failure(ends);
                }
                else if (*ends)
                {
                    break;
                }
            }
            check = {};
            ++sequence;
            continue;
        }
        if (header.block_type != JournalDescriptorBlock && header.block_type != JournalRevokeBlock)
        {
            break;
        }
        if (pass == LogPass::Scan && checksums && !JournalTailMatches(block, log.seed))
        {
            check.mismatch = true;
        }
        if (header.block_type == JournalRevokeBlock)
        {
            if (pass == LogPass::Scan)
            {
                check.outside = check.outside || !RevokeRecordsFit(block, log.header.incompatible);
            }
            else if (pass == LogPass::Revoke)
            {
                if (const Status added =
                        ReadRevokeRecords(block, log.header.incompatible, sequence, log.revokes);
                    !added)
                {
                    return Failure(added);
                }
            }
            continue;
        }
        if (const Status followed = FollowTags(log, pass, sequence, check, buffers); !followed)
        {
            return Failure(followed);
        }
    }
    return sequence;
}

// A commit block that fails its checksum ends the log only where commit blocks may be written
// before the rest of their transaction (async_commit); otherwise, as for a committed transaction
// with a block or descriptor that fails its checksum, Linux refuses to recover, and so does this.
Result<bool> Ext2Filesystem::CheckCommit(const JournalLog& log, std::uint32_t sequence,
                                         const TransactionCheck& check,
                                         std::span<const std::uint8_t> block)
{
    if (JournalHasChecksums(log.header.incompatible) && !JournalCommitMatches(block, log.seed))
    {
        if ((log.header.incompatible & JournalIncompatibleAsyncCommit) != 0)
        {
            LogWithValue("ext2: journal commit checksum mismatch, the log ends at ", sequence);
            return true;
        }
        LogWithValue("ext2: journal commit checksum mismatch; run e2fsck, transaction ", sequence);
        return std::unexpected(Error::Corrupt);
    }
    if (check.mismatch)
    {
        LogWithValue("ext2: journal checksum mismatch; run e2fsck, transaction ", sequence);
        return std::unexpected(Error::Corrupt);
    }
    if (check.outside)
    {
        LogWithValue("ext2: a journal transaction does not fit the volume: ", sequence);
        return std::unexpected(Error::Corrupt);
    }
    return false;
}

Status Ext2Filesystem::FollowTags(JournalLog& log, LogPass pass, std::uint32_t sequence,
                                  TransactionCheck& check, Buffers& buffers)
{
    const std::span<std::uint8_t> data = buffers[1];
    const std::uint32_t incompatible = log.header.incompatible;
    JournalTags tags(buffers[0], incompatible);
    while (const std::optional<JournalTag> tag = tags.Next())
    {
        const Result<std::uint32_t> position = log.Advance();
        if (!position)
        {
            return Failure(position);
        }
        if (pass == LogPass::Scan)
        {
            check.outside = check.outside || tag->block >= superblock.blocks_count;
        }
        if (pass == LogPass::Revoke ||
            (pass == LogPass::Scan && !JournalHasChecksums(incompatible)) ||
            (pass == LogPass::Replay && log.revokes.Revokes(tag->block, sequence)))
        {
            continue;
        }
        if (const Status read = ReadLogBlock(log, *position, data, buffers[2]); !read)
        {
            return read;
        }
        if (pass == LogPass::Scan)
        {
            check.mismatch =
                check.mismatch || !JournalTagMatches(*tag, incompatible, log.seed, sequence, data);
            continue;
        }
        if ((tag->flags & JournalTagEscaped) != 0)
        {
            StoreBigEndian32(data.data(), JournalMagic);
        }
        if (const Status written = cache.Write(tag->block, data); !written)
        {
            return written;
        }
    }
    return {};
}

Status Ext2Filesystem::ClearRecoveryFlag(std::span<std::uint8_t> scratch)
{
    const std::uint64_t block = SuperblockOffset / block_size;
    const std::size_t offset = SuperblockOffset % block_size;
    if (const Status read = cache.Read(block, 0, scratch, scratch); !read)
    {
        return read;
    }
    const std::span<std::uint8_t, SuperblockSize> bytes =
        scratch.subspan(offset).first<SuperblockSize>();
    const Superblock replayed = DecodeSuperblock(bytes);
    StoreLittleEndian(bytes.data() + SuperblockIncompatibleOffset,
                      replayed.incompatible & ~IncompatibleRecover);
    if (HasMetadataChecksums(replayed))
    {
        StoreSuperblockChecksum(bytes);
    }
    return cache.Write(block, scratch);
}

} // namespace FeatherDisk::Ext
