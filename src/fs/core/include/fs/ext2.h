// SPDX-License-Identifier: MIT OR Apache-2.0
#ifndef FEATHERDISK_EXT2_H
#define FEATHERDISK_EXT2_H

#include "fs/block_device.h"
#include "fs/error.h"
#include "fs/filesystem.h"

#include <cstdint>
#include <string_view>

namespace FeatherDisk::Ext
{

// ext2, ext3 and ext4 through one driver: ext2 revisions 0 and 1, with 1, 2 or 4 KiB blocks, and
// the ext3 and ext4 layout around it. ext3's journal is ignored. ext4 reads extent trees of any
// depth (unwritten extents as zeros), 64bit with large group descriptors, flex_bg, large inodes,
// huge_file, dir_nlink, extra_isize, unwritten groups (uninit_bg) and the CRC16 or CRC32C
// checksums (gdt_csum, metadata_csum and its seed), which are verified on read: a mismatch is
// Error::Corrupt. A journal that needs recovery is not replayed: the volume mounts read-only with a
// log line. Any other incompatible feature (meta_bg, inline_data, encrypt, casefold, mmp,
// journal_dev, ...) or bigalloc refuses the mount (Error::Unsupported, with a log line of the
// feature bits), and inodes flagged for inline data or encryption are Error::Unsupported. Node IDs
// are inode numbers, and the root is inode 2. Directories list in their on-disk order
// (hash-indexed ones too), names match byte for byte, symbolic links are not followed, and holes
// in sparse files read as zeros.
//
// A mount asked for writing creates, writes, truncates and removes files, makes and removes
// directories, renames, makes symbolic and hard links and sets attributes, on plain ext2 only. It
// falls back to read-only, with a log line saying why, for a journal or any ext4 feature, unknown
// read-only compatible or compatible features, a volume that was not cleanly unmounted or has
// errors recorded, inconsistent group descriptors or a device that refuses writes;
// GetCapabilities tells. Damage found while writing records errors in the superblock and, unless
// s_errors says continue, stops further writes. Every caller counts as root, so the reserved
// blocks are used too, and new files belong to user and group 0.
//
// Changes (writing operations and Sync) must not run at the same time as any other operation
// on the same mount; reads may run together, since the metadata cache's lock covers only copying.
//
// The three types differ only in Name and Probe: each probes only volumes of its own variant, as
// ExtFeatures::variant names them (so probing them in turn names a volume as blkid does), and each
// mounts all three variants, as Linux's ext4 driver does.
FilesystemType& Ext2Type();
FilesystemType& Ext3Type();
FilesystemType& Ext4Type();

enum class FeatureSet
{
    Compatible,
    Incompatible,
    ReadOnlyCompatible,
};

// What a volume's superblock says about its format.
struct ExtFeatures
{
    // "ext4" with any ext4 feature, else "ext3" with a journal, else "ext2".
    std::string_view variant;
    std::uint32_t compatible;
    std::uint32_t incompatible;
    std::uint32_t read_only_compatible;
};

// Error::Unsupported when the volume is not ext2, ext3 or ext4.
Result<ExtFeatures> DescribeFeatures(BlockDevice& volume);
// e2fsprogs' name for one feature bit, or an empty view for a bit without one.
std::string_view FeatureName(FeatureSet set, std::uint32_t bit);

} // namespace FeatherDisk::Ext

#endif
