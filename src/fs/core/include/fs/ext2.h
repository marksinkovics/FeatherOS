// SPDX-License-Identifier: MIT OR Apache-2.0
#ifndef FEATHERDISK_EXT2_H
#define FEATHERDISK_EXT2_H

#include "fs/filesystem.h"

namespace FeatherDisk::Ext
{

// ext2 ("ext2"), revisions 0 and 1, with 1, 2 or 4 KiB blocks. A volume with an incompatible
// feature other than filetype (extents, 64-bit block numbers, a journal that needs recovery, ...)
// does not mount (Error::Unsupported); an ext3 volume whose journal is clean reads as ext2. Node
// IDs are inode numbers, and the root is inode 2. Directories list in their on-disk order, names
// match byte for byte, symbolic links are not followed, and holes in sparse files read as zeros.
//
// A mount asked for writing creates, writes, truncates and removes files, makes and removes
// directories, renames, makes symbolic and hard links and sets attributes. It falls back to
// read-only, with a log line saying why, for unknown read-only compatible or compatible features,
// an ext3 journal, a volume that was not cleanly unmounted or has errors recorded, inconsistent
// group descriptors or a device that refuses writes; GetCapabilities tells. Damage found while
// writing records errors in the superblock and, unless s_errors says continue, stops further
// writes. Every caller counts as root, so the reserved blocks are used too, and new files belong to
// user and group 0.
//
// Changes (writing operations and Sync) must not run at the same time as any other operation
// on the same mount; reads may run together, since the metadata cache's lock covers only copying.
FilesystemType& Ext2Type();

} // namespace FeatherDisk::Ext

#endif
