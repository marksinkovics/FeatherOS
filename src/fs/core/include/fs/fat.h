// SPDX-License-Identifier: MIT OR Apache-2.0
#ifndef FEATHERDISK_FAT_H
#define FEATHERDISK_FAT_H

#include "fs/filesystem.h"

namespace FeatherDisk::Fat
{

// FAT16 on 512-byte sectors ("fat16"), with subdirectories and VFAT long names of up to 63
// printable ASCII characters; names match ignoring case. Every change goes straight to the
// volume, ordered so that an interrupted operation can leak clusters but never leaves an entry
// pointing at clusters it does not own. A mounted volume keeps nothing in memory that changes, so
// it takes no lock; the host runs operations that change it one at a time.
FilesystemType& Fat16Type();

// Whether FAT[1] of the volume has both of its status bits set (bit 15, a clean shutdown, and
// bit 14, no disk error found); the volume is only read. The core never writes these bits. A
// volume that is no FAT16 gives Error::Unsupported.
Result<bool> StatusBitsClean(BlockDevice& volume);

} // namespace FeatherDisk::Fat

#endif
