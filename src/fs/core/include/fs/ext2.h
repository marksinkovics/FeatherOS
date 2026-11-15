// SPDX-License-Identifier: MIT OR Apache-2.0
#ifndef FEATHERDISK_EXT2_H
#define FEATHERDISK_EXT2_H

#include "fs/filesystem.h"

namespace FeatherDisk::Ext
{

// ext2 ("ext2"), revisions 0 and 1, with 1, 2 or 4 KiB blocks, read-only: every mount is
// read-only whatever MountOptions ask, GetCapabilities reports it, and every change fails with
// Error::ReadOnly. A volume with an incompatible feature other than filetype (extents, 64-bit
// block numbers, a journal that needs recovery, ...) does not mount (Error::Unsupported); an ext3
// volume whose journal is clean reads as ext2. Node IDs are inode numbers, and the root is inode
// 2. Directories list in their on-disk order, names match byte for byte, symbolic links are not
// followed, and holes in sparse files read as zeros. Metadata blocks go through a small cache
// whose lock covers only copying, so operations may run at the same time.
FilesystemType& Ext2Type();

} // namespace FeatherDisk::Ext

#endif
