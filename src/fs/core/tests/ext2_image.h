// SPDX-License-Identifier: MIT OR Apache-2.0
// The ext2 images tests/make_ext2_image.sh makes, and e2fsprogs to check what the core wrote.
#ifndef FEATHERDISK_TESTS_EXT2_IMAGE_H
#define FEATHERDISK_TESTS_EXT2_IMAGE_H

#include "ext/journal.h"
#include "fs/filesystem.h"
#include "tests/memory_device.h"

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace FeatherDisk::Tests
{

constexpr std::uint32_t ImageSectorSize = 512;

// Only with FEATHERDISK_EXT_IMAGES.
std::vector<std::uint8_t> LoadExtImage(std::string_view variant);
std::unique_ptr<MemoryDevice> DeviceFrom(const std::vector<std::uint8_t>& bytes,
                                         std::uint32_t sector_size = ImageSectorSize);
std::unique_ptr<MemoryDevice> CopyOf(MemoryDevice& device);
std::vector<std::uint8_t> BytesOf(MemoryDevice& device);

// The volume's structures located from its bytes alone, to read or damage them on purpose.
struct RawVolume
{
    std::vector<std::uint8_t>& bytes;

    std::uint16_t Load16(std::uint64_t offset) const;
    std::uint32_t Load32(std::uint64_t offset) const;
    void Store16(std::uint64_t offset, std::uint16_t value);
    void Store32(std::uint64_t offset, std::uint32_t value);
    std::uint32_t BlockSize() const;
    std::uint32_t BlockCount() const;
    std::uint32_t InodeCount() const;
    std::uint64_t BlockOffset(std::uint32_t block) const;
    std::uint64_t DescriptorOffset() const;
    // 32 bytes, or s_desc_size with 64bit.
    std::uint32_t DescriptorSize() const;
    std::uint32_t InodeSize() const;
    std::uint64_t InodeOffset(std::uint32_t inode) const;
    // s_checksum again after a deliberate change, on a volume with metadata_csum.
    void UpdateSuperblockChecksum();
    std::uint32_t InodeBlock(std::uint32_t inode, std::uint32_t index) const;
    // The blocks of the extent tree nodes below the inode's root on the way to logical.
    std::vector<std::uint64_t> ExtentPath(std::uint32_t inode, std::uint32_t logical) const;
    // The block holding logical, 0 for a hole: through the extent tree, or one of the twelve
    // direct blocks of a block map.
    std::uint64_t FileBlock(std::uint32_t inode, std::uint32_t logical) const;
    // The named entry in the directory's first block.
    std::uint64_t EntryOffset(std::uint32_t directory, std::string_view name) const;
};

struct ToolRun
{
    bool ran;
    int status;
    std::string output;
};

// Whether e2fsck was found; without it, CheckClean passes and checks nothing.
bool HaveE2fsck();
// e2fsck -fn over the device's bytes, saved as <name>.img in the test output directory, which
// must exit 0: nothing to report.
void CheckClean(MemoryDevice& device, std::string_view name);
// e2fsck -fn's exit status and output, unchecked.
ToolRun RunE2fsck(MemoryDevice& device, std::string_view name);
// e2fsck -fp, preen mode: what it repairs without asking, read back into the device. Exit status
// 0 or 1 means nothing was left for a person to decide.
ToolRun PreenWithE2fsck(MemoryDevice& device, std::string_view name);
// What e2fsck may report about a volume that stopped in the middle of a change: lost blocks and
// inodes, counts, sizes and link counts behind the truth, unattached inodes and unconnected
// directories. With renames, also what a crash between a directory's two names leaves: a second
// name of a directory, and a ".." that still names the old parent. With indexes, the blocks of a
// hash index a directory was dropping, which stop e2fsck -n until preen mode salvages them.
bool OnlyRoutineRepairs(const std::string& report, bool renames = false, bool indexes = false);
// e2fsck -fy: what it repairs, orphans included, read back into the device.
ToolRun FixWithE2fsck(MemoryDevice& device, std::string_view name);
// The "Free blocks" and "Free inodes" line of every group in dumpe2fs's output.
std::string FreeRangesOf(MemoryDevice& device, std::string_view name);
// debugfs -R command over the device's bytes: what it printed on standard output.
ToolRun RunDebugfs(MemoryDevice& device, std::string_view name, std::string_view command);
// debugfs -c -R command: opened without the allocation bitmaps, whose checksums a volume stopped
// in the middle of a change may fail, which makes debugfs refuse the volume otherwise.
ToolRun InspectWithDebugfs(MemoryDevice& device, std::string_view name, std::string_view command);
// The inode debugfs's stat finds for path, without following a final symbolic link; nothing when
// the path does not exist.
std::optional<std::uint32_t> InodeOfPath(MemoryDevice& device, std::string_view name,
                                         std::string_view path);
// debugfs -w -R command, with the changed volume read back into the device.
ToolRun ChangeWithDebugfs(MemoryDevice& device, std::string_view name, std::string_view command);
// debugfs -w -f over a script of commands, one per line, with the changed volume read back into
// the device. "%N" in the script names a file holding files[N] (N a single digit).
ToolRun ScriptWithDebugfs(MemoryDevice& device, std::string_view name, std::string_view script,
                          const std::vector<std::vector<std::uint8_t>>& files);

// The journal's inode (8), where block n of the journal lies, and its superblock.
std::uint32_t JournalInode(const RawVolume& raw);
std::uint64_t JournalOffset(const RawVolume& raw, std::uint32_t n);
Ext::JournalSuperblock JournalSuperblockOf(MemoryDevice& device);

void RequireSameBytes(std::vector<std::uint8_t> ours, std::vector<std::uint8_t> theirs);
// debugfs's journal_run also writes s_wtime, s_lastcheck, s_kbytes_written and their high bytes,
// and the superblock checksum over them; a writable mount counts itself in s_mtime and s_mnt_count
// and sets the journal's features and checksum. Everything else must match.
void RequireSameAsDebugfs(MemoryDevice& ours, MemoryDevice& theirs);
// Replays the journal by mounting for writing, which must give a writable mount, and unmounting.
void ReplayByMounting(MemoryDevice& device);

// A volume's whole tree, to compare what a crash and its recovery left with what was committed.
struct TreeEntry
{
    NodeType type;
    // Zero for directories, which e2fsck may repack.
    std::uint64_t size;
    std::uint32_t links;
    // A file's bytes (left out of files over 8 MiB, which the image's own large files are), or a
    // symbolic link's target.
    std::string contents;
};

using Tree = std::map<std::string, TreeEntry>;

Tree TreeOf(Filesystem& filesystem);
// Mounted read-only for the length of the call.
Tree TreeOf(MemoryDevice& device);
// Where the two trees first differ (names, types, sizes, links, and contents when asked), or
// nothing when they are the same.
std::string TreeDifference(const Tree& left, const Tree& right, bool contents = true);

struct Replayed
{
    std::unique_ptr<MemoryDevice> ours;
    Tree tree;
    // Orphan entries our mount finished.
    std::uint32_t orphans = 0;
};

// Both ways of recovering a crashed volume: mounted writable (journal replay, then orphan
// processing) and e2fsck -fy. Each must leave a volume e2fsck -fn passes, and the two must hold
// the same tree and the same free block and inode ranges per group. Only ours when e2fsck is
// missing.
Replayed ReplayBothWays(MemoryDevice& crashed, std::string_view name);

} // namespace FeatherDisk::Tests

#endif
