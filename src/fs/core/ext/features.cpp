// SPDX-License-Identifier: MIT OR Apache-2.0
#include "ext/features.h"

#include "fs/ext2.h"

#include <array>
#include <bit>

namespace FeatherDisk::Ext
{
namespace
{

struct Named
{
    std::uint32_t bit;
    std::string_view name;
};

constexpr std::array<Named, 13> CompatibleNames{{
    {0x0001, "dir_prealloc"},
    {0x0002, "imagic_inodes"},
    {CompatibleHasJournal, "has_journal"},
    {CompatibleExtendedAttributes, "ext_attr"},
    {CompatibleResizeInode, "resize_inode"},
    {CompatibleDirectoryIndex, "dir_index"},
    {0x0040, "lazy_bg"},
    {0x0080, "snapshot_bitmap"},
    {0x0100, "exclude_bitmap"},
    {CompatibleSparseSuper2, "sparse_super2"},
    {0x0400, "fast_commit"},
    {0x0800, "stable_inodes"},
    {0x1000, "orphan_file"},
}};

constexpr std::array<Named, 16> IncompatibleNames{{
    {IncompatibleCompression, "compression"},
    {IncompatibleFileType, "filetype"},
    {IncompatibleRecover, "needs_recovery"},
    {IncompatibleJournalDevice, "journal_dev"},
    {IncompatibleMetaGroups, "meta_bg"},
    {IncompatibleExtents, "extent"},
    {Incompatible64Bit, "64bit"},
    {IncompatibleMultipleMountProtection, "mmp"},
    {IncompatibleFlexibleGroups, "flex_bg"},
    {IncompatibleExtendedAttributeInodes, "ea_inode"},
    {IncompatibleDirectoryData, "dirdata"},
    {IncompatibleChecksumSeed, "metadata_csum_seed"},
    {IncompatibleLargeDirectory, "large_dir"},
    {IncompatibleInlineData, "inline_data"},
    {IncompatibleEncryption, "encrypt"},
    {IncompatibleCaseFolding, "casefold"},
}};

constexpr std::array<Named, 16> ReadOnlyCompatibleNames{{
    {ReadOnlyCompatibleSparseSuper, "sparse_super"},
    {ReadOnlyCompatibleLargeFile, "large_file"},
    {ReadOnlyCompatibleBtreeDirectory, "btree_dir"},
    {ReadOnlyCompatibleHugeFile, "huge_file"},
    {ReadOnlyCompatibleGroupChecksum, "uninit_bg"},
    {ReadOnlyCompatibleDirectoryLinks, "dir_nlink"},
    {ReadOnlyCompatibleExtraInodeSize, "extra_isize"},
    {0x0080, "snapshot"},
    {0x0100, "quota"},
    {ReadOnlyCompatibleBigAlloc, "bigalloc"},
    {ReadOnlyCompatibleMetadataChecksum, "metadata_csum"},
    {0x0800, "replica"},
    {0x1000, "read-only"},
    {0x2000, "project"},
    {0x4000, "shared_blocks"},
    {0x8000, "verity"},
}};

template <std::size_t Count>
std::string_view Find(const std::array<Named, Count>& names, std::uint32_t bit)
{
    for (const Named& named : names)
    {
        if (named.bit == bit)
        {
            return named.name;
        }
    }
    return {};
}

} // namespace

std::string_view Variant(const Superblock& superblock)
{
    constexpr std::uint32_t Ext3Incompatible = IncompatibleCompression | IncompatibleFileType |
                                               IncompatibleRecover | IncompatibleJournalDevice |
                                               IncompatibleMetaGroups;
    constexpr std::uint32_t Ext3ReadOnlyCompatible = ReadOnlyCompatibleSparseSuper |
                                                     ReadOnlyCompatibleLargeFile |
                                                     ReadOnlyCompatibleBtreeDirectory;
    if ((superblock.incompatible & ~Ext3Incompatible) != 0 ||
        (superblock.read_only_compatible & ~Ext3ReadOnlyCompatible) != 0)
    {
        return "ext4";
    }
    if ((superblock.compatible & CompatibleHasJournal) != 0)
    {
        return "ext3";
    }
    return "ext2";
}

std::string_view FeatureName(FeatureSet set, std::uint32_t bit)
{
    if (!std::has_single_bit(bit))
    {
        return {};
    }
    switch (set)
    {
    case FeatureSet::Compatible:
        return Find(CompatibleNames, bit);
    case FeatureSet::Incompatible:
        return Find(IncompatibleNames, bit);
    case FeatureSet::ReadOnlyCompatible:
        return Find(ReadOnlyCompatibleNames, bit);
    }
    return {};
}

} // namespace FeatherDisk::Ext
