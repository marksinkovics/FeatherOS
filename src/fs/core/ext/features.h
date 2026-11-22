// SPDX-License-Identifier: MIT OR Apache-2.0
#ifndef FEATHERDISK_EXT_FEATURES_H
#define FEATHERDISK_EXT_FEATURES_H

#include "ext/superblock.h"

#include <string_view>

namespace FeatherDisk::Ext
{

// "ext4" with any feature ext3 did not have, else "ext3" with a journal, else "ext2".
std::string_view Variant(const Superblock& superblock);

} // namespace FeatherDisk::Ext

#endif
