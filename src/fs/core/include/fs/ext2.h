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
// the ext3 and ext4 layout around it. ext4 reads extent trees of any depth (unwritten extents as
// zeros), 64bit with large group descriptors, flex_bg, large inodes, huge_file, dir_nlink,
// extra_isize, unwritten groups (uninit_bg) and the CRC16 or CRC32C checksums (gdt_csum,
// metadata_csum and its seed), which are verified on read: a mismatch is Error::Corrupt. A jbd2
// journal inside the volume that needs recovery is replayed by a mount asked to write, which then
// goes on as for a clean volume. A read-only mount never writes, so it reads the volume as it is,
// with a log line, and so does a mount whose journal is on another device or has unknown
// features. A malformed journal fails the writable mount with Error::Corrupt before anything is
// written. Any other incompatible feature (meta_bg, inline_data, encrypt, casefold, mmp,
// journal_dev, ...) or bigalloc refuses the mount (Error::Unsupported, with a log line of the
// feature bits), and inodes flagged for inline data or encryption are Error::Unsupported. Node IDs
// are inode numbers, and the root is inode 2. Directories list in their on-disk order
// (hash-indexed ones too), names match byte for byte, symbolic links are not followed, and holes
// in sparse files read as zeros.
//
// A mount asked for writing creates, writes, truncates and removes files, makes and removes
// directories, renames, makes symbolic and hard links and sets attributes, on ext2 and on ext3 and
// ext4 layouts (extents, 64bit, flex_bg, checksums, hashed directories). A volume with an internal
// journal is written through it, in ordered mode: every change is one jbd2 transaction of
// metadata blocks (file data goes to its place first), which commits at Sync, at unmount, when it
// has grown to a quarter of the journal and, when the next change arrives, after 5 seconds; the
// host should call Sync every few seconds, which the core has no timer for. A change too big for
// one transaction even alone fails with Error::NoSpace and changes nothing. After an I/O error in
// the journal, this mount writes nothing more. needs_recovery stays set while the volume is
// mounted and goes at unmount. It falls back to read-only, with a log line saying why, for a
// journal on another device, of version 1 or shorter than 1024 blocks, or with features it
// does not know, any other feature whose rules it does not keep (meta_bg, inline_data, bigalloc,
// quota, orphans still to finish, ...), unknown read-only compatible or compatible features, a
// volume that was not cleanly unmounted or has errors recorded, inconsistent group descriptors or
// a device that refuses writes; GetCapabilities tells. Damage found while writing records errors
// in the superblock and, unless s_errors says continue, stops further writes. Every caller counts
// as root, so the reserved blocks are used too, and new files belong to user and group 0.
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
